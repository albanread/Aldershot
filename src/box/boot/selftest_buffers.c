/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_buffers.c: the Buffer Manager, rewritten in C, against the
 * behaviour that RISC OS 5's module documents (ROOL's Doc/BufferMan).
 *
 * The services are checked as clients use them:
 *
 *   - C code, through the typed API: api.h's SWIs and buffers.h's data
 *     operations.
 *   - Translated code, through register blocks: InsV, RemV and CnpV, and
 *     the direct-call routine Buffer_InternalInfo hands out.
 *   - Anyone, through shared memory. The addresses Buffer_GetInfo hands
 *     out are read directly. They must hold exactly the bytes the ring
 *     says they do.
 *
 * A randomised run then holds the ring to a model of the contract that
 * knows nothing of indices: a queue with a capacity one unit short of the
 * buffer's size.
 */
#include <stdio.h>
#include <string.h>

#include "buffers.h"
#include "rosgd/api.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

static const char *msg(const os_error *e)
{
    return e ? e->errmess : "no error";
}

static uint32_t errnum(const os_error *e)
{
    return e ? e->errnum : 0;
}

/* ---- owners, UpCalls and events, as the tests observe them ---------------- */

static struct {
    unsigned wakes, detaches;
    uint32_t r0, r8, r12;
    unsigned filling, emptying;
    unsigned output_empty, input_full;
    uint32_t event_r1, event_r3;
} seen;

static void wake_up(struct ros_cpu *s)
{
    seen.wakes++;
    seen.r0 = s->r[0];
    seen.r8 = s->r[8];
    seen.r12 = s->r[12];
    s->v = 0;
    s->r[15] = s->r[14];
}

static void detach_ok(struct ros_cpu *s)
{
    seen.detaches++;
    s->v = 0;
    s->r[15] = s->r[14];
}

static int on_upcall(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == 8)
        seen.filling++;
    if (s->r[0] == 9)
        seen.emptying++;
    return ROS_VECTOR_PASS;
}

static int on_event(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == 0)
        seen.output_empty++;
    if (s->r[0] == 1) {
        seen.input_full++;
        seen.event_r3 = s->r[3];
    }
    seen.event_r1 = s->r[1];
    return ROS_VECTOR_PASS;
}

/* A register block for a vector call, as translated code makes one. */
static int vector(uint32_t v, struct ros_cpu *s, uint32_t r0, uint32_t r1, uint32_t r2,
                  uint32_t r3, int c, int v_flag)
{
    ros_cpu_enter(s);
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    s->c = c;
    s->v = v_flag;
    return ros_vector_call(v, s);
}

/* The ring as Buffer_GetInfo shows it, read straight from shared memory,
 * against what the buffer should hold. */
static int ring_holds(uint32_t h, const uint8_t *want, uint32_t n)
{
    uint32_t flags, ins, rem, free_space, used;
    uint8_t *start, *end;
    if (xbuffer_get_info(h, &flags, &start, &end, &ins, &rem, &free_space, &used) || used != n)
        return 0;
    uint32_t size = (uint32_t)(end - start);
    for (uint32_t i = 0; i < n; i++)
        if (start[(rem + i) % size] != want[i])
            return 0;
    return 1;
}

/* ---- the randomised run ---------------------------------------------------- */

static uint32_t seed = 0x2545F491;

static uint32_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* Returns the number of the first operation to disagree, or 0. */
static unsigned randomized(uint32_t size, unsigned ops)
{
    uint32_t h, left;
    if (xbuffer_create(0, size, 0xFFFFFFFFu, &h))
        return 1;
    static uint8_t model[4200], data[4200], got[4200];
    uint32_t len = 0, capacity = size - 1;
    for (unsigned op = 1; op <= ops; op++) {
        uint32_t n = next() % (size + 8);
        switch (op % 5 == 2 ? 2 : next() % 5 == 2 ? 1 : next() % 5) {
        case 0: /* put */
            for (uint32_t i = 0; i < n; i++)
                data[i] = (uint8_t)next();
            if (xbuffer_put(h, data, n, &left))
                return op;
            uint32_t fits = n < capacity - len ? n : capacity - len;
            if (left != n - fits)
                return op;
            memcpy(model + len, data, fits);
            len += fits;
            break;
        case 1:   /* get */
        case 2: { /* peek */
            int peek = op % 5 == 2;
            if ((peek ? xbuffer_peek : xbuffer_get)(h, got, n, &left))
                return op;
            uint32_t had = n < len ? n : len;
            if (left != n - had || memcmp(got, model, had) != 0)
                return op;
            if (!peek) {
                memmove(model, model + had, len - had);
                len -= had;
            }
            break;
        }
        case 3: { /* count */
            uint32_t used, free_space;
            if (xbuffer_count(h, &used, &free_space) || used != len || free_space != capacity - len)
                return op;
            break;
        }
        case 4: /* now and then, empty it */
            if (next() % 8 == 0) {
                if (xbuffer_purge(h))
                    return op;
                len = 0;
            }
            break;
        }
        /* And a byte each way through InsV and RemV, as translated code
         * moves them: the byte paths, and the vector shims. */
        struct ros_cpu s;
        uint8_t byte = (uint8_t)next();
        if (!vector(ROS_INSV, &s, byte, h, 0, 0, 0, 0) || s.c != (len == capacity))
            return op;
        if (!s.c)
            model[len++] = byte;
        if (next() & 1) {
            if (!vector(ROS_REMV, &s, 0, h, 0, 0, 0, 0) || s.c != (len == 0) ||
                (!s.c && s.r[0] != model[0]))
                return op;
            if (!s.c)
                memmove(model, model + 1, --len);
        }
        if (!ring_holds(h, model, len))
            return op;
    }
    return xbuffer_remove(h) ? ops + 1 : 0;
}

/* ---- the checks ------------------------------------------------------------ */

void ros_selftest_buffers(void)
{
    os_error *e;
    uint32_t h, h2, h3, left, used, free_space;
    char what[128];

    ros_console_printf("rosgd: self-test -- the Buffer Manager, rewritten in C\n");

    struct ros_module *m = ros_module_for_swi(Buffer_Create);
    check(m && strcmp(m->title, "BufferManager") == 0 && m->swi_count == 10,
          "BufferManager: a native module, owning its SWI chunk", "%s", m ? m->title : "absent");
    const char *name = ros_swi_name(Buffer_InternalInfo);
    check(name && strcmp(name, "Buffer_InternalInfo") == 0, "BufferManager: its SWI names",
          "%s", name ? name : "none");

    /* ---- creating, and the errors on the way ---- */
    e = xbuffer_create(0, 16, 0xFFFFFFFFu, &h);
    check(!e && h == 256, "Buffer_Create: the first handle made is 256", "%s: %u", msg(e), h);
    e = xbuffer_create(0, 16, 0xFFFFFFFFu, &h2);
    check(!e && h2 == 257, "Buffer_Create: the next is the lowest unused", "%s: %u", msg(e), h2);
    e = xbuffer_create(0, 64, 1000, &h3);
    check(!e && h3 == 1000, "Buffer_Create: a handle asked for is given", "%s: %u", msg(e), h3);
    e = xbuffer_create(0, 64, 1000, &left);
    check(errnum(e) == 0x20705, "Buffer_Create: \"Buffer handle already in use\"", "%s", msg(e));
    e = xbuffer_create(0, 0, 0xFFFFFFFFu, &left);
    check(errnum(e) == 0x20706, "Buffer_Create: \"Buffer too small\"", "%s", msg(e));
    e = xbuffer_create(16, 10, 0xFFFFFFFFu, &left);
    check(errnum(e) == 0x20707, "Buffer_Create: \"Buffer must be word aligned\"", "%s", msg(e));
    uint32_t flags, ins, rem;
    uint8_t *start, *end;
    e = xbuffer_get_info(4242, &flags, &start, &end, &ins, &rem, &free_space, &used);
    check(errnum(e) == 0x20702, "Buffer_GetInfo: \"Buffer not known\"", "%s", msg(e));
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, XBuffer_Create + 12);
    e = s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
    check(errnum(e) == 0x20700 &&
              strcmp(msg(e), "SWI value out of range for module BufferManager") == 0,
          "Buffer SWI 12: the module's own \"SWI value out of range\"", "%s", msg(e));

    /* ---- shared memory: the ring is where GetInfo says, as it says ---- */
    e = xbuffer_get_info(h, &flags, &start, &end, &ins, &rem, &free_space, &used);
    check(!e && ros_in_arena(start) && ros_addr(start) >= ROS_RMA_BASE &&
              ros_addr(start) < ROS_RMA_BASE + ROS_RMA_SIZE && end - start == 16 &&
              ins == 0 && rem == 0 && free_space == 15 && used == 0,
          "Buffer_GetInfo: an empty 16-byte buffer in the RMA, 15 bytes free",
          "%s: start &%08X size %d ins %u rem %u free %u used %u", msg(e),
          start ? ros_addr(start) : 0, (int)(end - start), ins, rem, free_space, used);
    e = xbuffer_put(h, "HELLO", 5, &left);
    check(!e && left == 0 && ring_holds(h, (const uint8_t *)"HELLO", 5),
          "shared memory: \"HELLO\" is in the buffer, read directly", "%s", msg(e));
    e = xbuffer_put(h, "0123456789abcdefghij", 20, &left);
    xbuffer_count(h, &used, &free_space);
    check(!e && left == 10 && used == 15 && free_space == 0,
          "a full buffer: 15 of its 16 bytes, one kept free", "left %u used %u free %u", left,
          used, free_space);
    uint8_t got[32];
    e = xbuffer_get(h, got, 7, &left);
    check(!e && left == 0 && memcmp(got, "HELLO01", 7) == 0, "xbuffer_get: the oldest bytes first",
          "%.7s", got);
    e = xbuffer_put(h, "XYZ", 3, &left);
    check(!e && left == 0 && ring_holds(h, (const uint8_t *)"23456789XYZ", 11),
          "wrapping: the ring continues at the start of its memory", "%s", msg(e));
    e = xbuffer_peek(h, got, 4, &left);
    xbuffer_count(h, &used, NULL);
    check(!e && memcmp(got, "2345", 4) == 0 && used == 11, "xbuffer_peek: looks, leaves them",
          "%.4s, %u left", got, used);
    xbuffer_purge(h);
    xbuffer_count(h, &used, &free_space);
    check(used == 0 && free_space == 15, "xbuffer_purge: empty again", "%u %u", used, free_space);

    /* ---- the path translated code takes: vectors and the direct routine ---- */
    int claimed = vector(ROS_INSV, &s, 'A', h, 0, 0, 0, 0);
    check(claimed && !s.c, "InsV: a byte in, C clear", "claimed %d C %u", claimed, s.c);
    claimed = vector(ROS_REMV, &s, 0, h, 0, 0, 0, 1);
    check(claimed && !s.c && s.r[2] == 'A', "RemV with V set: examined, not removed",
          "C %u R2 %u", s.c, s.r[2]);
    claimed = vector(ROS_REMV, &s, 0, h, 0, 0, 0, 0);
    check(claimed && !s.c && s.r[0] == 'A' && s.r[2] == 'A', "RemV: the byte, in R0 and R2",
          "C %u R0 %u", s.c, s.r[0]);
    claimed = vector(ROS_REMV, &s, 0, h, 0, 0, 0, 0);
    check(claimed && s.c, "RemV on an empty buffer: C set", "C %u", s.c);

    uint8_t *arena = ros_rma_alloc(64);
    memcpy(arena, "block of eleven", 15);
    claimed = vector(ROS_INSV, &s, 0, h | 1u << 31, ros_addr(arena), 11, 0, 0);
    check(claimed && !s.c && s.r[3] == 0 && s.r[2] == ros_addr(arena) + 11,
          "InsV, bit 31: a block, R2 moved past it, R3 = 0", "C %u R3 %u", s.c, s.r[3]);
    claimed = vector(ROS_CNPV, &s, 0, h, 0, 0, 0, 0);
    check(claimed && s.r[1] == 11 && s.r[2] == 0, "CnpV: the count, split as OS_Byte 128's",
          "R1 %u R2 %u", s.r[1], s.r[2]);
    claimed = vector(ROS_CNPV, &s, 0, h, 0, 0, 1, 0);
    check(claimed && s.r[1] == 4, "CnpV with C set: the free space", "R1 %u", s.r[1]);
    claimed = vector(ROS_INSV, &s, 'q', 12, 0, 0, 0, 0);    /* (0-9 are the kernel's) */
    check(!claimed, "InsV for a buffer not the manager's: passed on", "claimed %d", claimed);
    ros_cpu_enter(&s);
    s.r[9] = ROS_REMV;
    s.r[1] = h | 1u << 31;
    s.r[2] = ros_addr(arena + 32);
    s.r[3] = 20;
    ros_swi(&s, XOS_CallAVector);
    check(!s.v && s.c && s.r[3] == 9 && memcmp(arena + 32, "block of el", 11) == 0,
          "OS_CallAVector RemV, bit 31: 11 bytes of 20, C set", "V %u C %u R3 %u", s.v, s.c,
          s.r[3]);

    uint32_t id, routine, workspace;
    e = xbuffer_internal_info(h, &id, &routine, &workspace);
    check(!e && routine >= ROS_NATIVE_BASE && ros_code_lookup(routine),
          "Buffer_InternalInfo: a routine compiled code can call", "%s: &%08X", msg(e), routine);
    ros_cpu_enter(&s);
    s.r[0] = 0; /* InsertByte */
    s.r[1] = id;
    s.r[2] = 'Z';
    s.r[12] = workspace;
    s.r[14] = 0xFC000004u;
    ros_call(&s, routine);
    int inserted = !s.v && !s.c && s.r[15] == 0xFC000004u;
    s.r[0] = 6; /* UsedSpace */
    s.r[14] = 0xFC000008u;
    ros_call(&s, routine);
    check(inserted && !s.v && s.r[2] == 1 && s.r[15] == 0xFC000008u,
          "the direct routine: InsertByte, then UsedSpace = 1", "V %u R2 %u", s.v, s.r[2]);
    s.r[0] = 10;
    ros_call(&s, routine);
    check(s.v && errnum(ros_ptr(s.r[0])) == 0x20708, "the direct routine: reason 10 is \"Bad parameters\"",
          "V %u", s.v);
    xbuffer_purge(h);

    /* ---- owners: woken when data arrives, asked before they are replaced ---- */
    uint32_t wake = ros_native_entry(wake_up, "test:wake_up");
    uint32_t detach = ros_native_entry(detach_ok, "test:detach");
    e = xbuffer_link_device(h, wake, detach, 0x1234, 0x5678);
    xbuffer_put(h, "a", 1, &left);
    xbuffer_put(h, "b", 1, &left);
    check(!e && seen.wakes == 1 && seen.r0 == h && seen.r8 == 0x1234 && seen.r12 == 0x5678,
          "Buffer_LinkDevice: woken once, with R0, R8, R12 as linked", "%s: %u wakes", msg(e),
          seen.wakes);
    xbuffer_get(h, got, 2, &left);
    xbuffer_put(h, "c", 1, &left);
    check(seen.wakes == 2, "emptied, dormant again: the next byte wakes it", "%u wakes",
          seen.wakes);
    e = xbuffer_link_device(h, 0, 0, 0, 0);
    check(!e && seen.detaches == 1, "a new owner: the old one's detach routine is asked",
          "%s: %u", msg(e), seen.detaches);
    xbuffer_count(h, &used, NULL);
    check(used == 0, "linking empties the buffer", "%u", used);
    e = xbuffer_link_device(h, wake, detach, 0, 0);
    check(errnum(e) == 0x20704, "an owner with no detach routine cannot be displaced",
          "%s", msg(e));
    e = xbuffer_unlink_device(h);
    check(!e, "Buffer_UnlinkDevice: always", "%s", msg(e));

    /* ---- the threshold: an UpCall each way as free space crosses it ---- */
    ros_vector_claim_native(ROS_UPCALLV, on_upcall, 0);
    uint32_t old, new_flags, previous;
    xbuffer_modify_flags(h, 8, 0xFFFFFFFFu, &old, &new_flags);
    xbuffer_threshold(h, 8, &previous);
    xbuffer_put(h, "1234567", 7, &left);       /* free 8: not below 8 */
    unsigned f0 = seen.filling;
    xbuffer_put(h, "8", 1, &left);             /* free 7: below */
    xbuffer_put(h, "9", 1, &left);             /* still below: no second UpCall */
    check(f0 == 0 && seen.filling == 1, "UpCall 8, once, as free space falls below the threshold",
          "%u then %u", f0, seen.filling);
    xbuffer_get(h, got, 1, &left);             /* free 7 */
    unsigned e0 = seen.emptying;
    xbuffer_get(h, got, 2, &left);             /* free 9: at or above */
    check(e0 == 0 && seen.emptying == 1, "UpCall 9, once, as it rises to the threshold again",
          "%u then %u", e0, seen.emptying);
    ros_vector_release_native(ROS_UPCALLV, on_upcall, 0);
    xbuffer_purge(h);

    /* ---- events: only while enabled, as the kernel's OSEVEN ---- */
    ros_vector_claim_native(ROS_EVENTV, on_event, 0);
    xbuffer_modify_flags(h, 2 | 4, 0xFFFFFFFFu, &old, &new_flags);
    xbuffer_put(h, "0123456789abcdefgh", 18, &left);
    check(seen.input_full == 0, "Event_InputFull: nothing while the event is disabled", "%u",
          seen.input_full);
    ros_event_enable(0, 1);
    ros_event_enable(1, 1);
    xbuffer_put(h, "ijk", 3, &left);
    check(seen.input_full == 1 && seen.event_r1 == h && seen.event_r3 == 3,
          "Event_InputFull: the handle, and the bytes that did not fit", "%u R1 %u R3 %u",
          seen.input_full, seen.event_r1, seen.event_r3);
    xbuffer_get(h, got, 32, &left);
    check(seen.output_empty == 1 && seen.event_r1 == h,
          "Event_OutputEmpty: as the last byte leaves", "%u", seen.output_empty);
    ros_event_enable(0, 0);
    ros_event_enable(1, 0);
    ros_vector_release_native(ROS_EVENTV, on_event, 0);

    /* ---- a client's own memory, and removal ---- */
    uint32_t h4;
    e = xbuffer_register(0, arena, arena + 64, 0xFFFFFFFFu, &h4);
    check(!e && h4 == 258, "Buffer_Register: a client's own memory as a buffer", "%s: %u",
          msg(e), h4);
    e = xbuffer_deregister(h4);
    check(!e, "Buffer_Deregister", "%s", msg(e));
    uint32_t in_use_before, in_use_after, spare;
    ros_rma_stats(&in_use_before, &spare);
    e = xbuffer_remove(h2);
    ros_rma_stats(&in_use_after, &spare);
    check(!e && in_use_after < in_use_before, "Buffer_Remove: the RMA it claimed is freed",
          "%s: %u -> %u", msg(e), in_use_before, in_use_after);
    ros_rma_free(arena);

    /* ---- the randomised run ---- */
    static const uint32_t sizes[] = { 2, 3, 16, 100, 4000 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        unsigned bad = randomized(sizes[i], 20000);
        snprintf(what, sizeof what, "randomized: 20,000 operations on a %u-byte buffer", sizes[i]);
        check(bad == 0, what, "disagreed with the model at operation %u", bad);
    }
    xbuffer_remove(h);
    xbuffer_remove(h3);
}
