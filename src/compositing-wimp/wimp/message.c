/* message.c: the message queue, Wimp_SendMessage and message lists.
 *
 * There is one queue for the desktop, first in, first out. An entry is
 * copied when it is sent. It is checked against its receiver only when it
 * reaches the head of the queue. The receiver must be alive, must accept
 * the message and must not mask it. An entry that fails the check is
 * dropped and is not kept.
 *
 * A recorded message (reason 18) becomes, once delivered, an
 * acknowledgement (reason 19) to its sender, in the same place in the
 * queue. A reply that names it deletes it first. Entries are RMA blocks,
 * so that the queue belongs to the desktop and not to any one thread. */
#include <string.h>

#include "rosgd/arena.h"
#include "rosgd/swi.h"
#include "wimp.h"

static uint32_t word(const uint8_t *b, unsigned off)
{
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}

static void set_word(uint8_t *b, unsigned off, uint32_t v)
{
    memcpy(b + off, &v, 4);
}

/* ---- the list --------------------------------------------------------------- */

static void append(struct wimp_ws *w, struct wimp_qentry *e)
{
    e->next = NULL;
    if (w->queue_tail)
        w->queue_tail->next = e;
    else
        w->queue = e;
    w->queue_tail = e;
}

static void unlink_entry(struct wimp_ws *w, struct wimp_qentry *e)
{
    struct wimp_qentry **p = &w->queue, *prev = NULL;
    while (*p && *p != e) {
        prev = *p;
        p = &(*p)->next;
    }
    if (!*p)
        return;
    *p = e->next;
    if (w->queue_tail == e)
        w->queue_tail = prev;
    xos_module_free(e);
}

void wimp_queue_discard(void)
{
    struct wimp_ws *w = wimp_ws();
    while (w && w->queue)
        unlink_entry(w, w->queue);
}

/* What was addressed to a deleted window goes to nobody. */
void wimp_queue_forget_window(uint32_t handle)
{
    struct wimp_ws *w = wimp_ws();
    for (struct wimp_qentry *e = w->queue; e; e = e->next)
        if (e->window == handle)
            e->recv_kind = RECV_NOBODY;
}

/* When a task closes down, what is queued for it goes too. */
void wimp_queue_forget_task(uint32_t handle)
{
    struct wimp_ws *w = wimp_ws();
    for (struct wimp_qentry *e = w->queue, *next; e; e = next) {
        next = e->next;
        if (e->recv_kind == RECV_TASK && e->recv == handle)
            unlink_entry(w, e);
    }
}

static uint32_t next_ref(struct wimp_ws *w)
{
    if (++w->my_ref == 0)                       /* skip 0, and keep the sign */
        w->my_ref = 1;
    return w->my_ref;
}

/* Queue an entry whose block is final, with the sender and my_ref
 * already in it. */
static os_error *queue(uint32_t reason, const uint8_t *block, uint32_t size, int recv_kind,
                       uint32_t recv, uint32_t sender)
{
    void *mem;
    os_error *e = xos_module_claim(sizeof(struct wimp_qentry), &mem);
    if (e)
        return e;
    struct wimp_qentry *q = mem;
    memset(q, 0, sizeof *q);
    q->reason = reason;
    q->recv_kind = recv_kind;
    q->recv = recv;
    q->sender = sender;
    q->size = size;
    if (size)
        memcpy(q->data, block, size);
    append(wimp_ws(), q);
    return NULL;
}

/* Queue a message or event from the Wimp itself. A user message is given
 * its sender and a my_ref, as one from a task is. */
os_error *wimp_queue_message(uint32_t reason, const uint8_t *block, uint32_t size,
                             int recv_kind, uint32_t recv, uint32_t sender)
{
    uint8_t b[256];
    if (sender == 0)
        wimp_clipboard_unpark();                /* a Wimp event: forget a held DataLoad */
    memcpy(b, block, size);
    if (reason >= 17 && reason <= 19) {
        set_word(b, 4, sender);
        set_word(b, 8, next_ref(wimp_ws()));
    }
    return queue(reason, b, size, recv_kind, recv, sender);
}

/* If a Message_SlotSize from this sender is already queued, and is not at
 * the head, it takes the new sizes. Returns 1 if there was one. */
int wimp_queue_update_slotsize(uint32_t sender, uint32_t current, uint32_t next)
{
    struct wimp_ws *w = wimp_ws();
    for (struct wimp_qentry *e = w->queue ? w->queue->next : NULL; e; e = e->next)
        if (e->reason == 17 && word(e->data, 16) == 0x400C4u && e->sender == sender) {
            set_word(e->data, 20, current);
            set_word(e->data, 24, next);
            return 1;
        }
    return 0;
}

/* ---- message lists --------------------------------------------------------------- */

int wimp_accepts(const struct wimp_task *t, uint32_t action)
{
    if (action == 0 || t->msgs_state == MSGS_ALL)
        return 1;                               /* Message_Quit always passes */
    if (t->msgs_state == MSGS_NONE)
        return 0;
    for (uint32_t i = 0; i < t->nmsgs; i++)
        if (t->msgs[i] == action)
            return 1;
    return 0;
}

void wimp_swi_AddMessages(struct ros_cpu *s)
{
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    uint32_t list = s->r[0];
    if (list && t->version >= 300 && t->msgs_state != MSGS_ALL) {
        if (t->msgs_state == MSGS_NONE) {
            t->msgs_state = MSGS_SET;
            t->nmsgs = 0;
        }
        for (uint32_t a; (a = ros_ld32(list)) != 0 && t->nmsgs < 64; list += 4) {
            int have = 0;
            for (uint32_t i = 0; i < t->nmsgs; i++)
                have |= t->msgs[i] == a;
            if (!have)
                t->msgs[t->nmsgs++] = a;
        }
    }
    s->v = 0;
}

void wimp_swi_RemoveMessages(struct ros_cpu *s)
{
    struct wimp_task *t = wimp_current();
    if (!t) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    uint32_t list = s->r[0];
    if (list && list != 0xFFFFFFFFu && t->version >= 300 && t->msgs_state == MSGS_SET) {
        for (uint32_t a; (a = ros_ld32(list)) != 0; list += 4)
            for (uint32_t i = 0; i < t->nmsgs; i++)
                if (t->msgs[i] == a) {
                    t->msgs[i] = t->msgs[--t->nmsgs];
                    break;
                }
    }
    s->v = 0;
}

/* ---- Wimp_SendMessage ------------------------------------------------------------ */

/* The number of bytes an event or message carries */
static uint32_t block_size(uint32_t reason, uint32_t block)
{
    switch (reason) {
    case 1: case 3: case 4: case 5: return 4;
    case 2: return 32;
    case 6: return 24;
    case 7: return 16;
    case 8: return 28;
    case 9: {
        uint32_t n = 0;
        while (n < 63 && (int32_t)ros_ld32(block + 4 * n) >= 0)
            n++;
        return 4 * (n + 1);
    }
    case 10: return ((ros_ld32(block + 32) | ros_ld32(block + 36)) & 3u) ? 40 : 44;
    case 11: case 12: return 24;
    case 17: case 18: case 19: return ros_ld32(block);
    default: return 0;
    }
}

void wimp_swi_SendMessage(struct ros_cpu *s)
{
    struct wimp_ws *w = wimp_ws();
    struct wimp_task *me = wimp_current();
    uint32_t reason = s->r[0], block = s->r[1], dest = s->r[2];
    if (!me) {
        wimp_fail(s, wimp_error(E_BAD_OP));
        return;
    }
    if (block < ROS_APP_BASE) {
        s->r[2] = 0;
        wimp_fail(s, wimp_error(E_BAD_PTR_R1));
        return;
    }
    if (reason >= 20) {
        s->r[2] = 0;
        wimp_fail(s, wimp_error(E_BAD_REASON));
        return;
    }
    uint32_t size = block_size(reason, block);
    if (reason == 19 && ((size & 3u) || size < 20 || size > 256)) {
        /* An acknowledgement is never queued, so its size is never looked
         * at (s/Wimp07 returns before calcmessagesize). Programs send one to
         * find the task behind a window before a transfer, with whatever is
         * in the block. OvationPro's drag to save does this (#145). Only the
         * header is read and written here. */
        size = 256;
    } else if (reason >= 17 && ((size & 3u) || size < 20 || size > 256)) {
        s->r[2] = 0;
        wimp_fail(s, wimp_error(E_BAD_MESSAGE));
        return;
    }

    wimp_clipboard_unpark();                    /* the task acted: forget a held DataLoad */
    if (reason >= 17 && size >= 28)             /* text for a writable icon */
        wimp_clipboard_intercept(ros_ptr(block), size, &dest);

    /* Find the receiver. */
    int recv_kind;
    uint32_t recv = 0, r2_out, window = 0;
    if (dest == 0) {
        recv_kind = RECV_BROADCAST;
        r2_out = 0;
    } else if ((dest & 3u) == 0 && (int32_t)dest > 0) {
        uint32_t internal = dest & 0xFFFFu;
        if (internal < 0x100u || ((internal - 0x100u) >> 2) >= WIMP_TASKS) {
            s->r[2] = 0;
            wimp_fail(s, wimp_error(E_BAD_TASK));
            return;
        }
        recv_kind = RECV_TASK;                  /* if dead: queued but never delivered */
        recv = dest;
        r2_out = dest;
    } else if (dest == 0xFFFFFFFEu) {
        /* The icon bar. The receiver is the task that created icon R3 on
         * it. If there is none, the message is queued for nobody. */
        uint32_t owner = (uint32_t)wimp_iconbar_owner((int32_t)s->r[3]);
        recv_kind = owner ? RECV_TASK : RECV_NOBODY;
        recv = owner;
        r2_out = owner;
        if (owner)
            window = w->iconbar;
    } else if ((int32_t)dest < 0) {
        if (reason <= 3) {                      /* a window of the Wimp's own */
            recv_kind = RECV_MAGIC;
            r2_out = WIMP_MAGIC;
        } else {
            recv_kind = RECV_NOBODY;
            r2_out = 0;
        }
    } else {                                    /* a window: its owner */
        struct wimp_window *win = wimp_window(dest);
        if (!win) {
            s->r[2] = 0;
            wimp_fail(s, wimp_error(E_BAD_HANDLE));
            return;
        }
        if ((int32_t)win->owner <= 0) {        /* a window of the Wimp's own */
            recv_kind = reason <= 3 ? RECV_MAGIC : RECV_NOBODY;
            r2_out = reason <= 3 ? WIMP_MAGIC : 0;
        } else {
            recv_kind = RECV_TASK;
            recv = win->owner;
            r2_out = win->owner;
        }
        window = dest;
    }

    uint8_t b[256];
    if (size)
        memcpy(b, ros_ptr(block), size);
    if (reason >= 17) {
        /* Delete the message this one replies to, set my_ref, and write
         * the sender and my_ref back to the caller's block. */
        uint32_t your_ref = word(b, 12);
        if (your_ref)
            for (struct wimp_qentry *e = w->queue; e; e = e->next)
                if (e->reason >= 17 && e->reason <= 19 && word(e->data, 8) == your_ref) {
                    unlink_entry(w, e);
                    break;
                }
        uint32_t ref = next_ref(w);
        set_word(b, 4, me->handle);
        set_word(b, 8, ref);
        if (block - ROS_ROM_BASE >= ROS_ROM_SIZE) {    /* "don't splat the ROM!" (s/Wimp07) */
            ros_st32(block + 4, me->handle);
            ros_st32(block + 8, ref);
        }
        if (reason == 19) {
            s->r[2] = r2_out;
            s->v = 0;
            return;
        }
    }
    os_error *e = queue(reason, b, size, recv_kind, recv, me->handle);
    if (!e && window)
        w->queue_tail->window = window;
    if (e) {
        s->r[2] = 0;
        wimp_fail(s, e);
        return;
    }
    s->r[2] = r2_out;
    s->v = 0;
}

/* ---- delivery ---------------------------------------------------------------------- */

/* The next live slot at or after from, or WIMP_TASKS if there is none */
static uint32_t live_slot(const struct wimp_ws *w, uint32_t from)
{
    for (uint32_t i = from; i < WIMP_TASKS; i++)
        if (w->task[i] && w->task[i]->live)
            return i;
    return WIMP_TASKS;
}

/* An entry that has been delivered to all its receivers. A recorded
 * message bounces back to its sender in the same place in the queue.
 * Anything else is freed. */
static void finish(struct wimp_ws *w, struct wimp_qentry *e)
{
    if (e->reason == 18) {
        e->reason = 19;
        e->recv_kind = e->sender ? RECV_TASK : RECV_NOBODY;
        e->recv = e->sender;
    } else {
        unlink_entry(w, e);
    }
}

/* Deliver the head of the queue, or drop it and try the next. Returns 1
 * with a receiver and its event, or 0 once the queue is empty. */
int wimp_queue_next(struct wimp_task **to, struct wimp_event *ev)
{
    struct wimp_ws *w = wimp_ws();
    while (w->queue) {
        struct wimp_qentry *e = w->queue;
        struct wimp_task *t = NULL;
        int last = 1;
        if (e->recv_kind == RECV_BROADCAST) {
            uint32_t slot = live_slot(w, e->cursor);
            if (slot == WIMP_TASKS) {           /* past the last slot */
                finish(w, e);
                continue;
            }
            t = w->task[slot];
            e->cursor = slot + 1;
            last = live_slot(w, slot + 1) == WIMP_TASKS;
        } else if (e->recv_kind == RECV_TASK) {
            t = wimp_task_by_handle(e->recv, (e->recv >> 21) != 0);
        } else if (e->recv_kind == RECV_MAGIC) {
            /* For a window of the Wimp's own. The Wimp takes no action
             * on it, and the entry is dropped. */
            unlink_entry(w, e);
            continue;
        }
        uint32_t code = e->reason;
        if (t && t->live)                       /* may hold a text DataLoad for the clipboard */
            wimp_clipboard_park(code, e->data, e->size, t->handle, e->sender);
        int given = t && t->live && !(t->mask & (1u << code)) &&
                    (code < 17 || wimp_accepts(t, word(e->data, 16)));
        if (given) {
            ev->reason = code;
            ev->size = (code == 0 || (code >= 13 && code <= 16)) ? 0 : e->size;
            if (ev->size)
                memcpy(ev->data, e->data, ev->size);
            ev->set_r2 = 1;
            ev->r2 = e->sender;
            w->last_sender = e->sender;
            *to = t;
        }
        if (!last)
            ;                                   /* a broadcast stays at the head */
        else
            finish(w, e);
        if (given)
            return 1;
    }
    return 0;
}
