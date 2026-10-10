/* task.c -- tasks, Wimp_Poll and the order of delivery.
 *
 * A Wimp task is a runtime task (task.h) with a record here.  Wimp_Poll
 * searches for the next thing to deliver, in a fixed order (see search).
 * The search runs on the thread of whichever task called Wimp_Poll.  What
 * it finds belongs to some task.  If it belongs to the caller, the poll
 * returns it.  If it belongs to another task, it is left in that task's
 * record and the baton is handed over (ros_task_switch).  The other task
 * was waiting in a Wimp_Poll of its own.  It takes the event up there and
 * returns into its program.  The caller waits in its switch until a later
 * search hands it something.
 *
 * So an event is copied into a task's poll block on the task's own thread,
 * with its own slot paged in by the runtime's switch.  The Wimp never
 * reaches into another task's memory to deliver an event. */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/environment.h"
#include "rosgd/meter.h"
#include "rosgd/platform.h"
#include "rosgd/streams.h"
#include "rosgd/swi.h"
#include "rosgd/task.h"
#include "rosgd/ticker.h"
#include "wimp.h"

#define POLL_IDLE_BIT  (1u << 20)
#define POLLWORD_BIT   (1u << 22)
#define FAST_BIT       (1u << 23)

/* ---- the task table ------------------------------------------------------- */

/* A slot's internal handle: non-zero, with bits 0-1 clear, and unique */
static uint32_t internal_of(uint32_t slot)
{
    return 0x100u + (slot << 2);
}

uint32_t wimp_slot_of(const struct wimp_task *t)
{
    return (t->internal - 0x100u) >> 2;
}

/* The record of the task now running.  This is the live record of its
 * domain, or NULL if the domain has none. */
struct wimp_task *wimp_current(void)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_task *rt = ros_task_current();
    if (!w)
        return NULL;
    for (unsigned i = 0; i < WIMP_TASKS; i++)
        if (w->task[i] && w->task[i]->live && w->task[i]->rt == rt)
            return w->task[i];
    return NULL;
}

/* A task by its external handle.  If the handle has no version bits, the
 * slot match takes any task in the handle's slot.  The live match needs
 * the version bits to match as well. */
struct wimp_task *wimp_task_by_handle(uint32_t handle, int live_match)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t internal = handle & 0xFFFFu;
    if (!w || internal < 0x100u || (internal & 3u))
        return NULL;
    uint32_t slot = (internal - 0x100u) >> 2;
    if (slot >= WIMP_TASKS || !w->task[slot] || !w->task[slot]->live)
        return NULL;
    struct wimp_task *t = w->task[slot];
    if ((handle >> 16) == 0 && !live_match)
        return t;
    return t->handle == handle ? t : NULL;
}

/* ---- the pollword list ------------------------------------------------------ */

static void pollword_join(struct wimp_ws *w, uint32_t slot)
{
    for (unsigned i = 0; i < w->npollwords; i++)
        if (w->pollwords[i] == slot)
            return;
    w->pollwords[w->npollwords++] = slot;
}

/* A leaving task's place is taken by the last entry */
static void pollword_leave(struct wimp_ws *w, uint32_t slot)
{
    for (unsigned i = 0; i < w->npollwords; i++)
        if (w->pollwords[i] == slot) {
            w->pollwords[i] = w->pollwords[--w->npollwords];
            return;
        }
}

/* ---- Wimp_Initialise ----------------------------------------------------------- */

static uint32_t normalise(uint32_t v)
{
    if (v < 300)
        return 200;
    if (v == 300 || v == 380)
        return v;
    return 310;
}

static uint32_t env_word(uint32_t handler)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = handler;
    c.r[1] = c.r[2] = c.r[3] = 0;
    ros_swi(&c, XOS_ChangeEnvironment);         /* read the handler */
    return c.r[1];
}

static uint32_t close_task(struct wimp_task *t, uint32_t why);
static void install_handlers(struct ros_task *rt);

/* Message_TaskInitialise, sent from the new task */
static void announce(struct wimp_task *t, uint32_t name)
{
    uint8_t b[256];
    uint32_t len = 0;
    while (len < 200 && ros_ld8(name + len) >= ' ')
        len++;
    uint32_t size = (28 + len + 1 + 3) & ~3u;
    memset(b, 0, sizeof b);
    ((uint32_t *)b)[0] = size;
    ((uint32_t *)b)[4] = 0x400C2u;
    ((uint32_t *)b)[5] = env_word(ROS_ENV_CAO);
    /* The memory it uses.  This is the number of its slot's pages that
     * have been touched (arena.h).  The slot's size is not used, because
     * it is 1.5 GB for most tasks. */
    ((uint32_t *)b)[6] = ros_task_slot_used(t->rt);
    t->used_told = ((uint32_t *)b)[6];
    uint32_t from = name, to = 28;
    if (ros_ld8(from) == '\\')
        from++;
    for (;;) {
        uint8_t c = ros_ld8(from++);
        b[to++] = c;
        if (c < ' ' || to >= size)
            break;
    }
    wimp_queue_message(17, b, size, RECV_BROADCAST, 0, t->handle);
}

/* The special keys set to give codes that the Wimp decodes */
static void keyboard_settings(void)
{
    static const uint8_t fx[][2] = { { 4, 2 }, { 219, 0x8A }, { 221, 2 }, { 222, 2 }, { 223, 2 },
                                     { 224, 2 }, { 225, 2 }, { 226, 2 }, { 227, 2 }, { 228, 2 },
                                     { 9, 0 }, { 10, 0 }, { 229, 1 } };
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
        if (fx[i][0] == 229 && w->singletask >= 0)
            break;                              /* no *FX 229 or 124 while single-tasking */
        ros_cpu_enter(&c);
        c.r[0] = fx[i][0], c.r[1] = fx[i][1], c.r[2] = 0;
        ros_swi(&c, XOS_Byte);
        if (!w->ntasks)
            w->oldfx[i] = (uint8_t)c.r[1];      /* kept for restorekeycodes (the command window's) */
    }
    if (w->singletask >= 0)
        return;
    ros_cpu_enter(&c);
    c.r[0] = 124;
    ros_swi(&c, XOS_Byte);
}

void wimp_swi_Initialise(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    int oldstyle = s->r[1] != TASK_WORD;       /* an old-style task */
    if (!oldstyle && s->r[0] < 200) {
        wimp_fail(s, wimp_error(E_BAD_VERSION));
        return;
    }
    struct ros_task *rt = ros_task_current();
    struct wimp_task *old = wimp_current();
    w->err.commandflag |= 0x80u;                /* the command window suspended meanwhile */
    if (old) {
        uint32_t e = close_task(old, 1);        /* the domain's old task closed first */
        if (e) {
            w->err.commandflag &= ~0x80u;       /* exitinit: un-suspended */
            wimp_fail(s, ros_ptr(e));
            return;
        }
    }

    /* A dead domain that initialises keeps its slot.  Otherwise it takes a
     * free one. */
    uint32_t slot = WIMP_TASKS;
    for (uint32_t i = 0; i < WIMP_TASKS && slot == WIMP_TASKS; i++)
        if (w->task[i] && !w->task[i]->live && w->task[i]->rt == rt)
            slot = i;
    for (uint32_t i = 0; i < WIMP_TASKS && slot == WIMP_TASKS; i++)
        if (!w->task[i] || (!w->task[i]->live && !w->task[i]->rt))
            slot = i;
    if (slot == WIMP_TASKS) {
        wimp_fail(s, ros_error(E_BAD_OP, "No room for another task"));
        return;
    }
    struct wimp_task *t = w->task[slot];
    if (!t) {
        void *block;
        os_error *e = xos_module_claim(sizeof *t, &block);
        if (e) {
            wimp_fail(s, e);
            return;
        }
        t = w->task[slot] = block;
    }
    memset(t, 0, sizeof *t);
    t->internal = internal_of(slot);
    t->generation = w->generation;
    w->generation = w->generation % 1023u + 1u;  /* bits 21-30, never 0 */
    t->handle = t->generation << 21 | t->internal;
    /* checkversion: an old-style task is the single-tasking program.  Its
     * version is 0, and no Message_ModeChange follows. */
    t->version = oldstyle ? 0 : normalise(s->r[0]);
    w->singletask = oldstyle ? (int32_t)slot : -1;
    if (oldstyle)
        w->mode_changed = 0;
    t->rt = rt;
    t->live = 1;
    uint32_t i = 0;
    for (uint32_t p = s->r[2]; p && i < sizeof t->name - 1 && ros_ld8(p) >= ' '; p++)
        t->name[i++] = (char)ros_ld8(p);
    t->name[i] = 0;

    /* the message list */
    t->msgs_state = MSGS_ALL;
    if (!oldstyle && t->version >= 300) {
        uint32_t list = s->r[3];
        if (list == 0)
            t->msgs_state = MSGS_NONE;
        else if (list != 0xFFFFFFFFu && ros_ld32(list) != 0) {
            t->msgs_state = MSGS_SET;
            for (uint32_t a; (a = ros_ld32(list)) != 0 && t->nmsgs < 64; list += 4)
                t->msgs[t->nmsgs++] = a;
        }
    }
    keyboard_settings();                        /* the special keys */
    int first = !w->ntasks;
    if (first)
        install_handlers(rt);                   /* the first task's handlers */
    if (first)
        wimp_windows_start();                   /* the first task */
    if (first && oldstyle) {                    /* int_allbutmode: no mode set */
        w->mode_changed = 0;
        wimp_tools_unlist();
        wimp_vdu_init();
        wimp_find_font();
        wimp_invalidate_box(wimp_screen_box());
        wimp_input_mode_set();
        for (int k = 0; k < 16; k++)            /* the colour mapping 1:1 */
            w->palette[k] = (w->palette[k] & ~0xFFu) | (uint32_t)k;
    }
    w->ntasks++;
    ros_task_set_domain(rt, t->internal);
    w->old_back = 0;                            /* "assume none yet" */
    if (oldstyle) {                             /* the screen covered, and no TaskInitialise */
        w->old_back = wimp_old_back_window(t->handle);
        w->err.commandflag &= ~0x80u;
        wimp_command_window(0xFFFFFFFFu);
        s->r[0] = WIMP_VERSION;
        s->v = 0;
        return;
    }
    announce(t, s->r[2]);
    /* The Wimp's mode is set again (int_setmode), unless the command
     * window is still pending.  That means a child that has printed
     * nothing, which 5.30's exitinit tests for.  Setting the mode puts the
     * screen into the Wimp's mode, sets the glyphs and finds the desktop
     * font again.  That sends Message_FontChanged, which for the first task
     * is its second one.  The screen is redrawn, and Message_ModeChange
     * follows at the next poll unless single-tasking. */
    if (w->err.commandflag != 0x81u)
        wimp_int_setmode(w->mode);
    (void)first;
    w->err.commandflag &= ~0x80u;               /* then the command window closed quietly */
    wimp_command_window(0xFFFFFFFFu);
    s->r[0] = WIMP_VERSION;
    s->r[1] = t->handle;
    s->v = 0;
}

/* ---- Wimp_CloseDown ------------------------------------------------------------ */

/* closedown: returns 0, or an error that a Service_WimpCloseDown claimant
 * gave (an R0 of 2 or more, s/Wimp02 closedown).  After an error the task
 * is left as it was. */
static uint32_t close_task(struct wimp_task *t, uint32_t why)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = why;                               /* Service_WimpCloseDown */
    c.r[1] = 0x53;
    c.r[2] = t->handle;
    ros_service_call(&c);
    if (c.r[0] >= 2)
        return c.r[0];                          /* an objection */
    wimp_queue_forget_task(t->handle);          /* its messages forgotten */
    wimp_menu_task_gone(t->handle);             /* its menus, */
    wimp_windows_of_task_delete(t->handle);     /* its windows, */
    wimp_iconbar_task_gone(t->internal);        /* and its iconbar icons */
    uint8_t b[20];
    memset(b, 0, sizeof b);
    ((uint32_t *)b)[0] = 20;
    ((uint32_t *)b)[4] = 0x400C3u;              /* Message_TaskCloseDown */
    wimp_queue_message(17, b, 20, RECV_BROADCAST, 0, t->handle);
    pollword_leave(w, wimp_slot_of(t));
    t->live = 0;
    t->pending = 0;
    w->ntasks--;
    if (!w->ntasks) {                           /* the last task gone */
        wimp_windows_end();
        wimp_clipboard_reset();                 /* the Clipboard Manager reset */
        wimp_keys_restore();                    /* the *FX settings saved at the first */
        if (w->singletask != (int32_t)wimp_slot_of(t)) {
            ros_cpu_enter(&c);                  /* the configured screen mode */
            c.r[0] = 1;
            ros_swi(&c, XOS_ReadSysInfo);
            if (!c.v) {
                uint32_t mode = c.r[0];
                ros_cpu_enter(&c);
                c.r[0] = 0, c.r[1] = mode;
                ros_swi(&c, XOS_ScreenMode);
            }
        }
    } else if (w->singletask != (int32_t)wimp_slot_of(t) && w->err.commandflag == 0 &&
               !ros_streams_own()) {
        /* Not for an SSH session's task.  What it prints next goes to its
         * terminal, and a command window left pending would catch the next
         * thing anyone printed (#164). */
        w->cmd_title[0] = 0;                    /* the command window pending, untitled */
        wimp_command_window(ros_addr(w->cmd_title));
    }
    return 0;
}

void wimp_swi_CloseDown(struct ros_cpu *s)
{
    struct wimp_task *t;
    int by_handle = s->r[1] == TASK_WORD;
    t = by_handle ? wimp_task_by_handle(s->r[0], 1) : wimp_current();
    uint32_t e = t ? close_task(t, 0) : 0;
    if (e) {
        wimp_fail(s, ros_ptr(e));
        return;
    }
    if (by_handle)
        s->r[1] = 0;
    s->v = 0;
}

/* A task's thread has ended (task.h, ros_task_on_end, registered by
 * wimp.c's init).  Its records can never run again, because the Wimp's
 * switch to an ended thread does nothing.  The Wimp's own children free
 * their records in dead_poll.  This is for the rest.  One such is an SSH
 * session's task, whose Wimp task ends with the session's command.  It
 * may end by the default handlers (an error, an Escape, or OS_Exit without
 * Wimp_CloseDown), or after closing down.  A live record left behind would
 * have its events switched to the dead thread for ever, and the box would
 * spin in its poll.  A dead record would keep its slot and its runtime
 * task's address.  The next session's task is often given that address.
 * That gives two live records for one thread, and two "Spinner"s in
 * *WimpStats.  Once that task is freed in turn, there is a switch to freed
 * memory ("task N: cannot map its slot", #164).  So the task is closed
 * down if it has not been, and its table entry is freed. */
void wimp_task_thread_ended(struct ros_task *rt)
{
    struct wimp_ws *w = wimp_ws();
    if (!w || !rt)
        return;
    if (w->first_rt == rt)
        w->first_rt = NULL;                     /* never to be parked in dead_poll now */
    for (uint32_t i = 0; i < WIMP_TASKS; i++) {
        struct wimp_task *t = w->task[i];
        if (!t || t->rt != rt)
            continue;
        if (t->live && close_task(t, 0) != 0) {
            /* an objection, which a thread that has gone cannot heed */
            wimp_queue_forget_task(t->handle);
            pollword_leave(w, i);
            t->live = 0;
            t->pending = 0;
            w->ntasks--;
        }
        if (w->singletask == (int32_t)i) {
            w->singletask = -1;
            w->old_back = 0;
        }
        t->rt = NULL;
    }
}

/* ---- the search ------------------------------------------------------------------ */

static int pollword_event(struct wimp_ws *w, int fast_only, struct wimp_task **to,
                          struct wimp_event *ev)
{
    for (unsigned i = 0; i < w->npollwords; i++) {
        struct wimp_task *t = w->task[w->pollwords[i]];
        if (!t || !t->live || !t->pollword || (fast_only && !t->pollword_fast))
            continue;
        if (t->mask & (1u << 13))
            continue;
        /* An acquire load.  Another thread may bump the word, as the
         * Worker module's pool does after its stores.  The acquire makes
         * what that thread wrote visible to the task afterwards. */
        uint32_t value = __atomic_load_n((uint32_t *)ros_ptr(t->pollword), __ATOMIC_ACQUIRE);
        if (!value)
            continue;
        ev->reason = 13;
        ev->size = 8;
        ((uint32_t *)ev->data)[0] = t->pollword;
        ((uint32_t *)ev->data)[1] = value;
        ev->set_r2 = 1;
        /* The fast pollword check runs before the last sender is reset,
         * so R2 is stale.  The check of all pollwords runs after it, so R2
         * is 0. */
        ev->r2 = fast_only ? w->last_sender : 0;
        *to = t;
        return 1;
    }
    return 0;
}

/* ---- null-event pacing (ROSGD's own, #138) ---------------------------------------- */

/* A task that takes null events back to back has its nulls held back
 * until the pacing interval has passed since its last one.  Back to back
 * means that it calls Wimp_Poll again within PACE_QUICK_NS of each null,
 * PACE_STREAK times in a row.  The poll then waits in the idle wait
 * (ros_idle), which any real event ends.  OvationPro blinks its caret that
 * way, at 130,000 polls a second.  These are not paced:
 *  - Wimp_PollIdle, whose own time is the pacing.
 *  - The single-tasking program.
 *  - A selection drag's end.
 *  - A task window's child, whose message list takes TaskWindow_Morite.
 *    Its nulls are its program's time slices.
 *  - A task that spends longer than PACE_QUICK_NS in its nulls.
 * The interval is read from Wimp$NullPace at most twice a second, or else
 * from the boot word rosgd.nullpace.  It is in milliseconds, or "<n>Hz".
 * 0 turns pacing off.  RISC OS 5.30 does not pace null events. */
#define PACE_DEFAULT_NS  5000000u         /* 5 ms: 200 nulls a second at most */
#define PACE_QUICK_NS    500000u          /* under 0.5 ms of the task's time per null */
#define PACE_STREAK      4u
#define TASKWINDOW_MORITE 0x808C4u

/* "5", "5ms", "200Hz": nanoseconds, or -1 if it is not a setting */
static int64_t pace_parse(const char *v)
{
    while (*v == ' ')
        v++;
    if (*v < '0' || *v > '9')
        return -1;
    char *end;
    unsigned long n = strtoul(v, &end, 10);
    if ((end[0] == 'H' || end[0] == 'h') && (end[1] == 'z' || end[1] == 'Z'))
        return n ? (int64_t)(1000000000ull / n) : 0;
    return n > 1000 ? 1000000000ll : (int64_t)n * 1000000;
}

static uint64_t pace_interval(struct wimp_ws *w)
{
    uint32_t cs = ros_monotonic_cs();
    if (w->pace_read && cs - w->pace_read_cs < 50)
        return w->pace_ns;
    if (!w->pace_read) {
        w->pace_boot_ns = PACE_DEFAULT_NS;
        if (ros_cmdline_has("rosgd.nullpace")) {
            const char *v = ros_cmdline_value("rosgd.nullpace");
            int64_t n = v ? pace_parse(v) : -1;
            w->pace_boot_ns = n < 0 ? 0 : (uint32_t)n;
        }
    }
    w->pace_read = 1;
    w->pace_read_cs = cs;
    w->pace_ns = w->pace_boot_ns;
    char *name = w->pace_var, *val = w->pace_var + 16;
    memcpy(name, "Wimp$NullPace", 14);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(name), c.r[1] = ros_addr(val), c.r[2] = 31, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    if (!c.v && c.r[2] < 32) {
        val[c.r[2]] = 0;
        int64_t n = pace_parse(val);
        if (n >= 0)
            w->pace_ns = (uint64_t)n;
    }
    return w->pace_ns;
}

static int taskwindow_child(const struct wimp_task *t)
{
    if (t->msgs_state != MSGS_SET)
        return 0;
    for (uint32_t i = 0; i < t->nmsgs; i++)
        if (t->msgs[i] == TASKWINDOW_MORITE)
            return 1;
    return 0;
}

/* At a poll's entry: how long the task took over its last null, and
 * whether this poll's nulls are paced */
static void pace_entry(struct wimp_ws *w, struct wimp_task *me, int idle, uint64_t now)
{
    if (me->null_out_ns) {
        if (now - me->null_out_ns < PACE_QUICK_NS) {
            if (me->null_quick < PACE_STREAK)
                me->null_quick++;
        } else {
            me->null_quick = 0;
        }
        me->null_out_ns = 0;
    }
    me->null_held = 0;
    me->null_paced = !idle && me->null_quick >= PACE_STREAK && !(me->mask & 1u) &&
                     w->singletask != (int32_t)wimp_slot_of(me) && pace_interval(w) &&
                     !taskwindow_child(me);
}

/* paced: 0 for a selection drag's end, whose null is never held */
static int null_event(struct wimp_ws *w, struct wimp_task **to, struct wimp_event *ev, int paced)
{
    uint32_t now = ros_monotonic_cs();
    if (w->singletask >= 0 && w->poller == w->singletask) {    /* single-tasking: only it */
        struct wimp_task *t = w->task[w->singletask];
        if (!t || !t->live || (t->mask & 1u))
            return 0;
        ev->reason = 0;
        ev->size = 0;
        ev->set_r2 = 1;
        ev->r2 = 0;
        *to = t;
        return 1;
    }
    uint64_t now_ns = 0;
    for (unsigned k = 1; k <= WIMP_TASKS; k++) {
        uint32_t slot = (w->null_last + k) % WIMP_TASKS;
        struct wimp_task *t = w->task[slot];
        if (!t || !t->live || (t->mask & 1u))
            continue;
        if ((t->mask & POLL_IDLE_BIT) && ((now - t->idle_time) & 0x80000000u))
            continue;
        if (t->null_paced && paced) {           /* held until its time */
            if (!now_ns)
                now_ns = ros_meter_now_ns();
            if (now_ns < t->null_due_ns) {
                t->null_held = 1;
                if (!w->pace_wake_ns || t->null_due_ns < w->pace_wake_ns)
                    w->pace_wake_ns = t->null_due_ns;
                continue;
            }
        }
        if (t->null_paced) {
            if (!now_ns)
                now_ns = ros_meter_now_ns();
            t->null_due_ns = now_ns + w->pace_ns;
        }
        w->null_last = slot;
        ev->reason = 0;
        ev->size = 0;
        ev->set_r2 = 1;
        ev->r2 = 0;
        *to = t;
        return 1;
    }
    return 0;
}

/* Nothing for anyone: the idle wait.  Any work queued ends it, such as
 * input or a Worker's completion.  It also ends at the time of the first
 * held null. */
static void idle_wait(struct wimp_ws *w)
{
    unsigned ms = 10;
    if (w->pace_wake_ns) {
        uint64_t now = ros_meter_now_ns();
        uint64_t rem = w->pace_wake_ns > now ? w->pace_wake_ns - now : 0;
        if (rem < 10000000u)
            ms = (unsigned)((rem + 999999u) / 1000000u);
    }
    ros_idle(ms);
}

/* The next thing to deliver, and to whom: 0 if there is nothing yet */
static int search(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    w->pace_wake_ns = 0;
    wimp_surface_poll();                        /* ROSGD: surface windows' changes */
    if (pollword_event(w, 1, to, ev))           /* fast pollwords first */
        return 1;
    if (wimp_queue_next(to, ev))                /* the message queue */
        return 1;
    w->hotkeyptr = 0;                           /* any hot key pass ended */
    if (w->mode_changed) {                      /* a mode change's messages */
        w->mode_changed = 0;
        wimp_mode_change_requests();
        if (wimp_queue_next(to, ev))
            return 1;
    }
    w->last_sender = 0;                         /* the last sender forgotten */
    if (wimp_redraw_scan(to, ev))               /* deferred opens, then redraw */
        return 1;
    if (pollword_event(w, 0, to, ev))           /* all pollwords */
        return 1;
    int r = wimp_input_event(to, ev);           /* the pointer, drags and buttons */
    if (r == 2)
        return null_event(w, to, ev, 0);        /* a selection drag's end */
    if (r == 3)
        return 0;                               /* ... and while it is held, nothing */
    if (r)
        return 1;
    if (wimp_key_event(to, ev))                 /* keys */
        return 1;
    if (wimp_wheel() && wimp_queue_next(to, ev))   /* ROSGD: the scroll wheel */
        return 1;
    return null_event(w, to, ev, 1);            /* null events last */
}

/* ---- Wimp_StartTask and dead tasks ------------------------------------------------- */

static void child_exit(struct ros_cpu *s);
static void child_error(struct ros_cpu *s);

static void entries(void)
{
    struct wimp_ws *w = wimp_ws();
    if (!w->exit_entry) {
        w->exit_entry = ros_native_entry(child_exit, "wimp:ExitHandler");
        w->error_entry = ros_native_entry(child_error, "wimp:ErrorHandler");
    }
}

static void handler(uint32_t n, uint32_t code, uint32_t r12, uint32_t buf, uint32_t *old)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = n, c.r[1] = code, c.r[2] = r12, c.r[3] = buf;
    ros_swi(&c, XOS_ChangeEnvironment);
    if (old)
        old[0] = c.r[1], old[1] = c.r[2], old[2] = c.r[3];
}

/* setdefaulthandlers: each handler that the application has not changed
 * since it started (handlerword) is reset.  The error and exit handlers
 * are set to the Wimp's own, and the others to OS_ReadDefaultHandler's.
 * The CAO is left alone.  RISC OS sets it to the Wimp's module base, but
 * the native module has no image, so this differs from 5.30. */
static void set_default_handlers(void)
{
    struct wimp_ws *w = wimp_ws();
    for (uint32_t n = 0; n <= ROS_ENV_UPCALL; n++) {
        if (n == ROS_ENV_CAO || (w->handlerword >> n & 1))
            continue;
        if (n == ROS_ENV_ERROR) {
            handler(n, w->error_entry, 0, ros_addr(w->errbuf), NULL);
        } else if (n == ROS_ENV_EXIT) {
            handler(n, w->exit_entry, 0, 0, NULL);
        } else {
            struct ros_cpu c;
            ros_cpu_enter(&c);
            c.r[0] = n;
            ros_swi(&c, XOS_ReadDefaultHandler);
            if (!c.v)
                handler(n, c.r[1], c.r[2], c.r[3], NULL);
        }
    }
}

/* The first task's exit handler becomes the Wimp's.  The one it replaces
 * is kept for when the last task dies.  Then the defaults are set. */
static void install_handlers(struct ros_task *rt)
{
    struct wimp_ws *w = wimp_ws();
    entries();
    w->first_rt = rt;
    handler(ROS_ENV_EXIT, w->exit_entry, 0, 0, w->saved_exit);
    set_default_handlers();
}

/* The domain's dead record (its pending block), or NULL */
static struct wimp_task *dead_record(struct ros_task *rt)
{
    struct wimp_ws *w = wimp_ws();
    for (unsigned i = 0; i < WIMP_TASKS; i++)
        if (w->task[i] && !w->task[i]->live && w->task[i]->rt == rt)
            return w->task[i];
    return NULL;
}

/* Ended children's threads freed */
static void reap(void)
{
    struct wimp_ws *w = wimp_ws();
    for (uint32_t i = 0; i < w->nreap;) {
        if (ros_task_ended(w->reap[i])) {
            ros_task_destroy(w->reap[i]);
            w->reap[i] = w->reap[--w->nreap];
        } else {
            i++;
        }
    }
}

/* The parent waiting in Wimp_StartTask is released before the polling
 * task looks for events.  It is given the handle of the task that polled,
 * or 0 if that task is dead.  The poller waits until an event is found
 * for it. */
static void release_parent(struct wimp_task *me)
{
    struct wimp_ws *w = wimp_ws();
    while (w->nparents) {
        struct wimp_task *p = w->task[w->parents[--w->nparents]];
        if (!p || !p->live)
            continue;                           /* a parent that died is dropped */
        p->start_result = me ? me->handle : 0;
        ros_task_switch(p->rt);
        return;
    }
}

/* The poll of a task whose domain is dead.  Its record is freed and a
 * waiting parent is released.  Otherwise the baton goes to whichever task
 * has the next event.  The domain's thread ends here and never returns. */
static void dead_poll(void)
{
    struct wimp_ws *w = wimp_ws();
    struct ros_task *self = ros_task_current();
    struct wimp_task *d = dead_record(self);
    uint32_t was = w->err.commandflag;          /* lookfornewtask's R7 */
    wimp_command_window(0);                     /* "Press SPACE" if anything was printed */
    if (d) {
        uint32_t slot = wimp_slot_of(d);
        if (w->singletask == (int32_t)slot) {
            w->singletask = -1;
            w->old_back = 0;                    /* deleted with its windows */
        }
        d->rt = NULL;                           /* the table entry free again */
    }
    if (w->ntasks) {
        /* The next task is switched to with the screen reset, which means
         * the Wimp's mode is set again.  That is skipped if the command
         * window was pending, with nothing printed.  Then the key codes are
         * set again (s/Wimp03, from lookfornewtask). */
        if (was != CF_PENDING)
            wimp_int_setmode(w->mode);
        keyboard_settings();
    }
    if (self == w->first_rt) {
        /* The first task's domain is the program that started the
         * desktop.  It is task 0's thread, which cannot end.  It parks and
         * hands the baton on until the last task has gone.  Then it exits
         * through the exit handler that the Wimp replaced. */
        while (w->ntasks) {
            struct wimp_task *to;
            struct wimp_event ev;
            if (search(&to, &ev)) {
                to->ev = ev;
                to->pending = 1;
                ros_task_switch(to->rt);
                continue;
            }
            idle_wait(w);
        }
        w->first_rt = NULL;
        handler(ROS_ENV_EXIT, w->saved_exit[0], w->saved_exit[1], w->saved_exit[2], NULL);
        struct ros_cpu c;
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_Exit);
        for (;;)
            ros_idle(1000);
    }
    if (w->nreap < sizeof w->reap / sizeof w->reap[0])
        w->reap[w->nreap++] = self;
    while (w->nparents) {
        struct wimp_task *p = w->task[w->parents[--w->nparents]];
        if (!p || !p->live)
            continue;
        p->start_result = 0;
        ros_task_exit(p->rt);
    }
    if (!w->ntasks)
        ros_task_exit(w->first_rt);             /* no task left: the first domain resumes */
    for (;;) {
        struct wimp_task *to;
        struct wimp_event ev;
        if (search(&to, &ev)) {
            to->ev = ev;
            to->pending = 1;
            ros_task_exit(to->rt);
        }
        idle_wait(w);
    }
}

/* The Wimp's exit handler for its children.  The task is closed down, then
 * it goes into the dead task's poll. */
static void child_exit(struct ros_cpu *s)
{
    (void)s;
    struct wimp_task *me = wimp_current();
    if (me)
        close_task(me, 0);
    dead_poll();
}

/* Its error handler.  The error is printed on the console and reported
 * with Wimp_ReportError, then the task ends as it would by its exit. */
static void child_error(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t num;
    memcpy(&num, w->errbuf + 4, 4);
    ros_console_printf("wimp: a task's error: %s (&%X)\n", (const char *)w->errbuf + 8, num);
    struct ros_cpu c;                           /* the report */
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(w->errbuf) + 4, c.r[1] = 2u | 4u, c.r[2] = 0xFFFFFFFFu;
    ros_swi(&c, XWimp_ReportError);
    child_exit(s);
}

/* The child's thread.  The Wimp's handlers are set and its placeholder is
 * closed down.  Then its command is run. */
static void child_main(void *arg)
{
    struct wimp_ws *w = wimp_ws();
    uint32_t command = (uint32_t)(uintptr_t)arg;
    struct ros_cpu c;
    w->handlerword = 0;                         /* a new application (Wimp07 StartTask) */
    w->parentquit = 0;
    set_default_handlers();
    ros_cpu_enter(&c);
    ros_swi(&c, XWimp_CloseDown);               /* the placeholder */
    if (!ros_ld8(command)) {                    /* the Clipboard Manager */
        wimp_clipboard_main();
        ros_cpu_enter(&c);
        ros_swi(&c, XOS_Exit);
        dead_poll();
    }
    if (w->clip.state == CB_PENDING) {
        /* A temporary "WindowManager" task starts the Clipboard Manager
         * and closes down.  Then the command is run. */
        ros_cpu_enter(&c);
        c.r[0] = 200, c.r[1] = TASK_WORD, c.r[2] = wimp_clipboard_title();
        ros_swi(&c, XWimp_Initialise);
        if (!c.v) {
            uint32_t temp = c.r[1];
            ros_cpu_enter(&c);
            c.r[0] = wimp_clipboard_title();
            ros_swi(&c, XWimp_StartTask);
            ros_cpu_enter(&c);
            c.r[0] = temp, c.r[1] = TASK_WORD;
            ros_swi(&c, XWimp_CloseDown);
        }
    }
    ros_cpu_enter(&c);
    c.r[0] = command;
    ros_swi(&c, XOS_CLI);
    if (c.v)
        ros_env_raise(ros_ptr(c.r[0]));         /* to the error handler */
    ros_cpu_enter(&c);
    ros_swi(&c, XOS_Exit);
    dead_poll();
}

void wimp_swi_StartTask(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    reap();                                     /* a parent may start several children before they poll */
    struct wimp_task *me = wimp_current();
    if (!me) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    if (w->singletask == (int32_t)wimp_slot_of(me)) {
        wimp_fail(s, ros_error(0x299, "Can't start task from here"));
        return;
    }
    uint32_t slot = WIMP_TASKS;
    for (uint32_t i = 0; i < WIMP_TASKS && slot == WIMP_TASKS; i++)
        if (!w->task[i] || (!w->task[i]->live && !w->task[i]->rt))
            slot = i;
    if (slot == WIMP_TASKS) {
        wimp_fail(s, ros_error(0x290, "Too many tasks"));
        return;
    }
    int clipboard = s->r[0] == wimp_clipboard_title();     /* 5.30's Title test */
    if (clipboard)
        w->clip.state = CB_RUN;
    uint32_t n = 0;                             /* the command, at most 1,023 characters */
    while (n < 1024 && ros_ld8(s->r[0] + n) >= 32)
        n++;
    if (n >= 1024) {
        wimp_fail(s, ros_error(0x299, "Can't start task from here"));
        return;
    }
    void *cmd;
    os_error *e = xos_module_claim(n + 1, &cmd);
    if (e) {
        wimp_fail(s, e);
        return;
    }
    memcpy(cmd, ros_ptr(s->r[0]), n);
    ((char *)cmd)[n] = 0;
    struct wimp_task *t = w->task[slot];
    if (!t) {
        void *block;
        if ((e = xos_module_claim(sizeof *t, &block))) {
            xos_module_free(cmd);
            wimp_fail(s, e);
            return;
        }
        t = w->task[slot] = block;
    }
    entries();
    /* the placeholder, as the single-tasking program */
    memset(t, 0, sizeof *t);
    t->internal = internal_of(slot);
    t->generation = w->generation;
    w->generation = w->generation % 1023u + 1u;
    t->handle = t->generation << 21 | t->internal;
    t->version = 0;
    t->live = 1;
    t->msgs_state = MSGS_ALL;
    if (clipboard)
        ((char *)cmd)[0] = 0;                   /* no command: the Clipboard Manager's code */
    struct ros_task *rt = ros_task_create(ros_slot_next_size, child_main,
                                          (void *)(uintptr_t)ros_addr(cmd));
    if (!rt) {
        t->live = 0;
        xos_module_free(cmd);
        wimp_fail(s, ros_error(0x290, "Too many tasks"));
        return;
    }
    t->rt = rt;
    ros_task_set_domain(rt, t->internal);
    w->ntasks++;
    w->singletask = (int32_t)slot;
    if (clipboard)                              /* 5.30 leaves the first command its title */
        wimp_command_window(ros_addr(w->cmd_title));
    else
        wimp_command_pending(s->r[0]);          /* the command window pending */
    w->parents[w->nparents++] = wimp_slot_of(me);   /* the parent kept */
    me->start_result = 0;
    uint32_t r1 = s->r[1];
    ros_task_switch(rt);                        /* the child runs until this task is released */
    wimp_filter_post(0, r1, me->internal);      /* the post filters see a null, and a claim is ignored */
    s->r[0] = me->start_result;
    s->v = 0;
}

/* ---- Wimp_Poll and Wimp_PollIdle ------------------------------------------------- */

static void dead_poll(void) __attribute__((noreturn));
static void slot_use_changed(void);

static void poll(struct ros_cpu *s, int idle)
{
    struct wimp_ws *w = wimp_ws();
    reap();
    uint64_t entry_ns = ros_meter_now_ns();     /* before the Wimp does anything, for pacing */
    struct wimp_task *me = wimp_current();
    if (!me) {
        if (dead_record(ros_task_current()))
            dead_poll();                        /* never returns */
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    wimp_menu_poll_entry();                     /* a chosen menu tree closes */
    wimp_clipboard_poll_entry();                /* a parked DataLoad passed on */
    if (s->r[1] < ROS_APP_BASE) {
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    /* the pre filters, before anything else */
    uint32_t mask = wimp_filter_pre(s->r[0] | (idle ? (1u << 20) : 0), s->r[1], me->internal) & ~(1u << 20),
             pollword = 0;
    if (me->version >= 300) {
        if (mask & (1u << 13))
            mask &= ~POLLWORD_BIT;
        if (mask & POLLWORD_BIT) {
            pollword = s->r[3];
            uint32_t top = ROS_APP_BASE + ros_task_slot_size(me->rt);
            if ((pollword & 3u) || (pollword >= ROS_APP_BASE && pollword <= top)) {
                wimp_fail(s, wimp_error(E_BAD_R3));
                return;
            }
        }
        if (mask & (1u << 21 | 0xFE000000u)) {
            wimp_fail(s, wimp_error(E_BAD_SYSINFO));
            return;
        }
    }
    if (w->singletask == (int32_t)wimp_slot_of(me))
        mask |= 0x000FF800u;                    /* single-tasking: no codes 11-19 */
    me->mask = (mask & 0x000FFFFFu) | (idle ? POLL_IDLE_BIT : 0);
    me->block = s->r[1];
    me->idle_time = s->r[2];
    me->pollword = pollword;
    me->pollword_fast = pollword && (mask & FAST_BIT);
    if (pollword)
        pollword_join(w, wimp_slot_of(me));
    pace_entry(w, me, idle, entry_ns);          /* null-event pacing */
    release_parent(me);                         /* a waiting parent released, before the search */
    slot_use_changed();

    struct wimp_event ev;
again:
    for (;;) {
        if (me->pending) {                      /* chosen for us by another's search */
            ev = me->ev;
            me->pending = 0;
            if (ev.reason == WIMP_EV_EDIT && !wimp_edit_key(ev.r2, &ev))
                continue;
            break;
        }
        struct wimp_task *to;
        w->poller = (int32_t)wimp_slot_of(me);
        int found = search(&to, &ev);
        w->poller = -1;
        if (found) {
            if (to == me) {
                if (ev.reason == WIMP_EV_EDIT && !wimp_edit_key(ev.r2, &ev))
                    continue;
                break;
            }
            to->ev = ev;
            to->pending = 1;
            ros_task_switch(to->rt);
            continue;
        }
        ros_streams_idle();                     /* other command lines run.  An SSH
                                                 * session only gets the baton while
                                                 * nothing in the box has work. */
        idle_wait(w);                           /* nothing for anyone: wait */
    }

    /* into the task's block, on its own thread and slot */
    if (ev.size)
        memcpy(ros_ptr(me->block), ev.data, ev.size);
    /* the post filters, which may change the event or claim it */
    uint32_t reason = wimp_filter_post(ev.reason, me->block, me->internal);
    if (reason == 0xFFFFFFFFu)
        goto again;
    pollword_leave(w, wimp_slot_of(me));
    if (reason == 0) {                          /* the task's time with the null starts */
        me->nulls++;
        me->nulls_paced += me->null_held != 0;
        me->null_out_ns = ros_meter_now_ns();
    }
    me->null_held = 0;
    s->r[0] = reason;
    if (ev.set_r2 && me->version >= 300)
        s->r[2] = ev.r2;
    s->v = 0;
}

void wimp_swi_Poll(struct ros_cpu *s)
{
    poll(s, 0);
}

void wimp_swi_PollIdle(struct ros_cpu *s)
{
    poll(s, 1);
}

/* ---- Wimp_SlotSize -------------------------------------------------------------- */

static uint32_t pages(uint32_t size)
{
    return size > 0xFFFFF000u ? 0xFFFFF000u : (size + 4095u) & ~4095u;
}

static uint32_t free_memory(void)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 5;                                 /* OS_DynamicArea 5: the free pool */
    c.r[1] = 0xFFFFFFFFu;
    ros_swi(&c, XOS_DynamicArea);
    return c.v ? 0 : c.r[2];
}

/* Message_SlotSize from t, or the one already queued from it updated.
 * Its current size is the memory the task uses, which is the pages of its
 * slot that have been touched (arena.h).  That is what the Task Manager
 * shows for it.  Wimp_SlotSize gives the task its slot's size. */
static void tell_slot(struct wimp_task *t, uint32_t next)
{
    uint32_t current = ros_task_slot_used(t->rt);
    t->used_told = current;
    uint8_t b[28];
    memset(b, 0, sizeof b);
    ((uint32_t *)b)[0] = 28;
    ((uint32_t *)b)[4] = 0x400C4u;
    ((uint32_t *)b)[5] = current;
    ((uint32_t *)b)[6] = next;
    if (!wimp_queue_update_slotsize(t->handle, current, next))
        wimp_queue_message(17, b, 28, RECV_BROADCAST, 0, t->handle);
}

/* The current task's slot changed size */
static void announce_slot(struct wimp_task *t, uint32_t current, uint32_t next)
{
    wimp_ws()->appspacesize = current;          /* sendmemmessage's appspacesize */
    tell_slot(t, next);
}

/* The tasks' memory in use, told again as it changes.  It is looked at
 * once a second, as tasks poll.  It is told for each task whose use has
 * moved by 16K or more since it was last told.  A program touching its
 * slot is not an event the Wimp sees, and a task waiting in Wimp_Poll
 * does not poll again.  So the Task Manager learns of the change this way,
 * from whichever task polls. */
static void slot_use_changed(void)
{
    static uint32_t looked;
    uint32_t now = ros_monotonic_cs();
    if (now - looked < 100)
        return;
    looked = now;
    struct wimp_ws *w = wimp_ws();
    for (uint32_t i = 0; i < WIMP_TASKS; i++) {
        struct wimp_task *t = w->task[i];
        if (!t || !t->live || !t->rt)
            continue;
        uint32_t used = ros_task_slot_used(t->rt);
        uint32_t by = used > t->used_told ? used - t->used_told : t->used_told - used;
        if (by >= 16u << 10)
            tell_slot(t, ros_slot_next_size);
    }
}

void wimp_swi_SlotSize(struct ros_cpu *s)
{
    struct wimp_task *me = wimp_current();
    struct ros_task *rt = ros_task_current();
    if (s->r[1] != 0xFFFFFFFFu)                 /* the next size first */
        ros_task_slot_next(pages(s->r[1]));
    uint32_t before = env_word(ROS_ENV_MEMORY_LIMIT) - ROS_APP_BASE;
    /* the current task's slot, or a dead domain's (a !Run file's
     * *WimpSlot, before its program initialises) */
    if (s->r[0] != 0xFFFFFFFFu && (me || dead_record(rt)) &&
        env_word(ROS_ENV_MEMORY_LIMIT) == env_word(ROS_ENV_APPLICATION_SPACE)) {
        uint32_t want = pages(s->r[0]);
        if (want > ROS_APP_LIMIT - ROS_APP_BASE)
            want = ROS_APP_LIMIT - ROS_APP_BASE;    /* as much as there can be */
        if (want != ros_task_slot_size(rt) && want)
            ros_task_resize_space(want);        /* as far as memory allows */
    }
    uint32_t now = env_word(ROS_ENV_MEMORY_LIMIT) - ROS_APP_BASE;
    if (now != before && me)
        announce_slot(me, now, ros_slot_next_size);
    s->r[0] = now;
    s->r[1] = ros_slot_next_size;
    s->r[2] = free_memory();                    /* the box's real free memory (dynarea.c) */
    s->v = 0;
}

/* Wimp_Extend 15 (WimpExtend_ReadSlotSize, Wimp 5.67): Wimp_SlotSize's
 * three figures in pages.  R0 is the current slot, R1 the next slot and R2
 * the free memory.  The Task Manager reads these in place of
 * Wimp_SlotSize's.  It shows Free as R2 less R1, because on RISC OS the
 * next slot comes out of the free pool.  Here the next slot is lazy
 * (arena.h) and takes none of it.  So R2 has the next slot added back, and
 * the Task Manager's Free is the box's real free memory.  In pages the
 * figure cannot look negative, as Wimp_SlotSize's bytes would past 2 GB.
 * So this is the one place it is added. */
void wimp_extend_read_slot_size(struct ros_cpu *s)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = c.r[1] = 0xFFFFFFFFu;
    ros_swi(&c, XWimp_SlotSize);
    s->r[0] = c.r[0] >> 12;
    s->r[1] = c.r[1] >> 12;
    s->r[2] = (c.r[2] >> 12) + s->r[1];
}

/* servicememorymoved: the application space changed under the current
 * task.  Message_SlotSize is sent if the task is alive. */
void wimp_slot_moved(uint32_t size)
{
    struct wimp_task *me = wimp_current();
    wimp_ws()->appspacesize = size;
    if (me && me->live)
        announce_slot(me, size, ros_slot_next_size);
}

/* ---- Wimp_ClaimFreeMemory ------------------------------------------------------------- */

#define CLAIM_MAX (4u << 20)

void wimp_swi_ClaimFreeMemory(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    s->v = 0;
    if (s->r[0] == 0) {                         /* released, whoever asks */
        w->claim_lent = 0;
        return;
    }
    if (w->claim_lent) {                        /* already lent */
        s->r[1] = 0, s->r[2] = 0;
        return;
    }
    uint32_t most = free_memory();
    if (most > CLAIM_MAX)
        most = CLAIM_MAX;
    if (s->r[1] > most) {                       /* too much: the most there is */
        s->r[1] = most, s->r[2] = 0;
        return;
    }
    struct ros_cpu c;
    if (!w->claim_area) {                       /* an area of the Wimp's own */
        char *name = (char *)w->scratch + 448;
        strcpy(name, "Wimp_ClaimFreeMemory workspace");
        ros_cpu_enter(&c);
        c.r[0] = 0, c.r[1] = 0xFFFFFFFFu, c.r[2] = 0, c.r[3] = 0xFFFFFFFFu;
        c.r[4] = 0x81u, c.r[5] = CLAIM_MAX, c.r[6] = c.r[7] = 0, c.r[8] = ros_addr(name);
        ros_swi(&c, XOS_DynamicArea);
        if (c.v) {
            s->r[1] = s->r[2] = 0;
            return;
        }
        w->claim_area = c.r[1], w->claim_base = c.r[3];
    }
    ros_cpu_enter(&c);
    c.r[0] = 2, c.r[1] = w->claim_area;
    ros_swi(&c, XOS_DynamicArea);               /* its size now */
    uint32_t size = c.v ? 0 : c.r[2];
    if (size < s->r[1]) {
        ros_cpu_enter(&c);
        c.r[0] = w->claim_area, c.r[1] = s->r[1] - size;
        ros_swi(&c, XOS_ChangeDynamicArea);
        if (c.v) {
            s->r[1] = s->r[2] = 0;
            return;
        }
        size += c.r[1];
    }
    w->claim_lent = 1;                          /* lent */
    s->r[1] = size, s->r[2] = w->claim_base;
}

/* ---- Wimp_TransferBlock --------------------------------------------------------------- */

/* A range in a task's slot must end within its MemoryLimit.  A range at
 * or above the top of application space is taken as it is. */
static int range_ok(struct wimp_task *t, uint32_t start, uint32_t len)
{
    if (start < ROS_APP_BASE)
        return 0;
    if (start >= ROS_APP_LIMIT)
        return 1;
    uint32_t limit = t == wimp_current() ? env_word(ROS_ENV_MEMORY_LIMIT)
                                         : ROS_APP_BASE + ros_task_slot_size(t->rt);
    return start + len <= limit;
}

void wimp_swi_TransferBlock(struct ros_cpu *s)
{
    struct wimp_task *dst = wimp_task_by_handle(s->r[2], 1), *src = wimp_task_by_handle(s->r[0], 1);
    if (!dst || !src) {
        wimp_fail(s, wimp_error(E_BAD_TASK));
        return;
    }
    uint32_t from = s->r[1], to = s->r[3], len = s->r[4];
    if ((int32_t)len < 0 || !range_ok(src, from, len) || !range_ok(dst, to, len)) {
        wimp_fail(s, ros_error(0x29D, "Wimp transfer out of range"));
        return;
    }
    s->v = 0;
    if (!len)
        return;
    int src_app = from < ROS_APP_LIMIT, dst_app = to < ROS_APP_LIMIT;
    if ((src == dst || !src_app) && (src == dst || !dst_app)) {
        /* one slot, or none: a memmove with that slot at &8000 */
        struct wimp_task *t = src_app ? src : dst;
        const struct ros_slot *was = ros_task_page_in(t->rt);
        memmove(ros_ptr(to), ros_ptr(from), len);
        ros_task_page_back(was);
        return;
    }
    /* two slots: through a buffer, a piece at a time */
    static uint8_t buf[64 << 10];
    while (len) {
        uint32_t n = len < sizeof buf ? len : (uint32_t)sizeof buf;
        const struct ros_slot *was = ros_task_page_in(src_app ? src->rt : NULL);
        memcpy(buf, ros_ptr(from), n);
        ros_task_page_back(was);
        was = ros_task_page_in(dst_app ? dst->rt : NULL);
        memcpy(ros_ptr(to), buf, n);
        ros_task_page_back(was);
        from += n, to += n, len -= n;
    }
}

/* ---- *WimpSlot ---------------------------------------------------------------------- */

/* A size: OS_ReadUnsigned in base 10, then K, M or G */
static int read_size(uint32_t text, uint32_t *size)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 10;
    c.r[1] = text;
    ros_swi(&c, XOS_ReadUnsigned);
    if (c.v)
        return 0;
    uint64_t v = c.r[2];
    uint8_t k = ros_ld8(c.r[1]);
    if (k == 'K' || k == 'k')
        v <<= 10, c.r[1]++;
    else if (k == 'M' || k == 'm')
        v <<= 20, c.r[1]++;
    else if (k == 'G' || k == 'g')
        v <<= 30, c.r[1]++;
    if (ros_ld8(c.r[1]) > ' ' || v > 0xFFFFFFFFu)
        return 0;
    *size = (uint32_t)v;
    return 1;
}

static uint32_t slot_size(uint32_t r0, uint32_t r1)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = r0;
    c.r[1] = r1;
    ros_swi(&c, XWimp_SlotSize);
    return c.r[0];
}

os_error *wimp_cmd_wimpslot(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    void *buffer;
    os_error *e = xos_module_claim(256, &buffer);
    if (e)
        return e;
    /* the keywords and OS_ReadArgs' output, both in the arena */
    uint32_t keys = ros_addr(buffer), out = keys + 16;
    memcpy(buffer, "min,max,next", 13);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = keys;
    c.r[1] = tail;
    c.r[2] = out;
    c.r[3] = 240;
    ros_swi(&c, XOS_ReadArgs);
    uint32_t arg[3], have[3] = { 0, 0, 0 }, size[3];
    int bad = c.v;
    for (int i = 0; i < 3 && !bad; i++) {
        arg[i] = ros_ld32(out + 4 * i);
        if (arg[i]) {
            have[i] = 1;
            bad = !read_size(arg[i], &size[i]);
        }
    }
    xos_module_free(buffer);
    if (bad)
        return ros_error(0x16B, "Bad parameters");
    uint32_t current = env_word(ROS_ENV_MEMORY_LIMIT) - ROS_APP_BASE;
    if (have[0] && size[0] > current) {                 /* -min: grown to it */
        current = slot_size(size[0], 0xFFFFFFFFu);
        if (current < size[0])
            return ros_error(0x1C1, "%uK free memory is needed before the application will "
                                    "start. Quit any unwanted applications or see the RISC OS "
                                    "User Guide for ways to maximise memory.", size[0] >> 10);
    }
    if (have[1] && size[1] < current)                   /* -max: shrunk to it */
        slot_size(size[1], 0xFFFFFFFFu);
    if (have[2])                                        /* -next */
        slot_size(0xFFFFFFFFu, size[2]);
    return NULL;
}
