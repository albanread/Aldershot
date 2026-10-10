/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation in C of RISC OS Open's Buffer Manager
 * (Sources/HWSupport/Buffers: s.Buffers, s.Buffer, s.MoveBlock, s.Errors, hdr.Buffer).
 */

/* buffers.c: the Buffer Manager, rewritten for ROSGD as a native module.
 *
 * It provides the Buffer Manager's services as RISC OS 5's module documents
 * them (ROOL's Doc/BufferMan, the routines' entry comments and hdr/Buffer).
 * The services are its SWIs, its InsV, RemV and CnpV claims, its
 * direct-call routine, its events and its UpCalls. It is new code written
 * from that documentation. It is not a translation.
 *
 * Clients depend on the documented interface by address, so this module
 * keeps what it fixes exactly:
 *
 *   - Buffers are shared memory. They live in the RMA, at one address in
 *     every task. Buffer_GetInfo hands out their addresses and indices,
 *     and clients read the bytes directly.
 *   - The ring is therefore the one RISC OS uses. The bytes from rem up to
 *     but not including ins are the data. If ins equals rem the buffer is
 *     empty. One byte is always left free, or one word for a word-aligned
 *     buffer, so that a full buffer is never mistaken for an empty one.
 *   - A producer and a consumer run concurrently. On RISC OS they were an
 *     interrupt handler and a task, and interrupts were disabled to keep
 *     them apart. Here an inserter writes the data and then publishes ins.
 *     A remover reads the data and then publishes rem. Both use C11
 *     atomics. The management calls (create, link and purge) will run
 *     under the runtime's personality lock once there is more than one
 *     thread.
 *   - All the module's state is in the RMA too. It is reached through the
 *     module's private word, and nothing is kept in C statics (module.h).
 *
 * The interface for C is typed. The SWIs are in api.h. The data operations
 * (put, get, peek, count and purge) are in buffers.h. The register-level
 * entry points are InsV, RemV, CnpV and the direct-call routine from
 * Buffer_InternalInfo. They are thin shims over the same functions, for
 * code translated from ObjAsm. The direct-call routine was used by
 * DeviceFS's drivers, and it may be retired.
 */
#include <stdatomic.h>
#include <string.h>

#include "buffers.h"
#include "rosgd/api.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"

/* The buffer flags. The low five belong to the client (hdr/Buffer). */
#define F_NOT_DORMANT      (1u << 0)
#define F_OUTPUT_EMPTY     (1u << 1)     /* Generate Event_OutputEmpty */
#define F_INPUT_FULL       (1u << 2)     /* Generate Event_InputFull */
#define F_THRESHOLD_UPCALL (1u << 3)     /* Send UpCalls as free space crosses the threshold */
#define F_WORD_ALIGNED     (1u << 4)
#define F_CLIENT           0x1Fu
#define F_EXCEEDED         (1u << 8)     /* Free space is below the threshold */

#define BLOCK_BIT          (1u << 31)    /* On a handle: a block transfer */
#define FIRST_HANDLE       256u          /* Generated handles count up from here */
#define NO_HANDLE          0xFFFFFFFFu   /* Marks a free slot */
#define DETACH_REFUSE      1u            /* The owner cannot be detached */

enum { EVENT_OUTPUT_EMPTY = 0, EVENT_INPUT_FULL = 1 };
enum { UPCALL_FILLING = 8, UPCALL_EMPTYING = 9 };
#define SERVICE_BUFFER_STARTING 0x6Fu

/* Errors. The numbers are RISC OS's, from ErrorBase_BufferManager (&20700).
 * The texts are those of the module's UK Messages file. */
#define ERR_BAD_SWI         0x20700u
#define ERR_TOO_MANY        0x20701u
#define ERR_BAD_BUFFER      0x20702u
#define ERR_IN_USE          0x20703u
#define ERR_UNABLE_TO_DETACH 0x20704u
#define ERR_HANDLE_USED     0x20705u
#define ERR_TOO_SMALL       0x20706u
#define ERR_NOT_ALIGNED     0x20707u
#define ERR_BAD_PARM        0x20708u

/* ---- the state, all in the RMA ------------------------------------------ */

struct record {
    uint32_t handle;                    /* NO_HANDLE means a free slot */
    _Atomic uint32_t flags;
    uint32_t start;                     /* Arena address of the buffer */
    uint32_t size;
    _Atomic uint32_t ins;               /* Written only by inserters */
    _Atomic uint32_t rem;               /* Written only by removers */
    uint32_t wake_up;                   /* The owner's routines are code */
    uint32_t detach;                    /*   addresses, or 0, or DETACH_REFUSE */
    uint32_t private;                   /* R8 for both routines */
    uint32_t workspace;                 /* R12 for both routines */
    uint32_t threshold;
};

struct workspace {
    uint32_t table;                     /* Arena address of the records */
    uint32_t count;                     /* Number of records, free slots included */
    uint32_t routine;                   /* The direct-call routine */
};

struct ros_module buffer_manager_module;

static struct workspace *ws(void)
{
    return ros_ptr(ros_ld32(buffer_manager_module.private_word));
}

static struct record *records(const struct workspace *w)
{
    return w->table ? ros_ptr(w->table) : NULL;
}

/* Find a live buffer by handle. Bit 31 is ignored, as in the original's
 * findbuffer. */
static struct record *find(uint32_t handle)
{
    struct workspace *w = ws();
    struct record *r = records(w);
    handle &= ~BLOCK_BIT;
    for (uint32_t i = 0; i < w->count; i++)
        if (r[i].handle == handle)
            return &r[i];
    return NULL;
}

static os_error *bad_buffer(void)
{
    return ros_error(ERR_BAD_BUFFER, "Buffer not known");
}

/* ---- calling out: owners, events, UpCalls ------------------------------- */

static os_error *call_owner(uint32_t code, const struct record *r)
{
    if (code == 0)
        return NULL;
    if (code == DETACH_REFUSE)
        return ros_error(ERR_UNABLE_TO_DETACH, "Unable to detach current owner of this buffer");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = r->handle;
    s.r[8] = r->private;
    s.r[12] = r->workspace;
    ros_call(&s, code);
    if (s.r[15] != ROS_RETURN_TO_NATIVE)
        ros_bad_return(&s, ROS_RETURN_TO_NATIVE);
    return s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
}

static void event(uint32_t n, uint32_t handle, uint32_t r2, uint32_t r3)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = n;
    s.r[1] = handle;
    s.r[2] = r2;
    s.r[3] = r3;
    ros_event_generate(&s);
}

static void upcall(struct record *r, uint32_t reason)
{
    if (reason == UPCALL_FILLING)
        atomic_fetch_or(&r->flags, F_EXCEEDED);
    else
        atomic_fetch_and(&r->flags, ~F_EXCEEDED);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = reason;
    s.r[1] = r->handle;
    s.r[2] = reason == UPCALL_FILLING ? 0 : 0xFFFFFFFFu;
    ros_vector_call(ROS_UPCALLV, &s);
}

/* Data entered a dormant buffer: mark it awake, then tell its owner. */
static void wake(struct record *r)
{
    if (!(atomic_fetch_or(&r->flags, F_NOT_DORMANT) & F_NOT_DORMANT))
        call_owner(r->wake_up, r);
}

static void make_dormant(struct record *r)
{
    atomic_fetch_and(&r->flags, ~F_NOT_DORMANT);
}

/* ---- the ring ------------------------------------------------------------ */

/* (a - b) modulo size, for indices and offsets in [0, size]. */
static uint32_t ring(int64_t a, int64_t b, uint32_t size)
{
    int64_t d = a - b;
    return (uint32_t)(d < 0 ? d + size : d);
}

static uint32_t unit(uint32_t flags)
{
    return flags & F_WORD_ALIGNED ? 4 : 1;
}

static uint32_t used_space(const struct record *r)
{
    return ring(atomic_load(&r->ins), atomic_load(&r->rem), r->size);
}

/* Free space, as GetInfo and FreeSpace give it: less one unit, the gap. */
static uint32_t free_space(struct record *r)
{
    uint32_t flags = atomic_load(&r->flags);
    return ring((int64_t)atomic_load(&r->rem) - unit(flags), atomic_load(&r->ins), r->size);
}

/* The UpCall checks, made after an insertion or a removal. The first
 * looks for free space that has fallen below the threshold. The second
 * looks for free space that has risen to the threshold again. */
static void check_filling(struct record *r, uint32_t free_now)
{
    uint32_t flags = atomic_load(&r->flags);
    if ((flags & (F_THRESHOLD_UPCALL | F_EXCEEDED)) == F_THRESHOLD_UPCALL &&
        free_now < r->threshold)
        upcall(r, UPCALL_FILLING);
}

static void check_emptying(struct record *r, uint32_t free_now)
{
    uint32_t flags = atomic_load(&r->flags);
    if ((flags & (F_THRESHOLD_UPCALL | F_EXCEEDED)) == (F_THRESHOLD_UPCALL | F_EXCEEDED) &&
        free_now >= r->threshold)
        upcall(r, UPCALL_EMPTYING);
}

/* 1 if the byte could not be inserted: the buffer is full. */
static int insert_byte(struct record *r, uint8_t byte)
{
    uint32_t size = r->size;
    uint32_t ins = atomic_load_explicit(&r->ins, memory_order_relaxed);
    uint32_t rem = atomic_load_explicit(&r->rem, memory_order_acquire);
    uint32_t next = ins + 1 == size ? 0 : ins + 1;
    if (next == rem) {
        if (atomic_load(&r->flags) & F_INPUT_FULL)
            event(EVENT_INPUT_FULL, r->handle, byte, 0);
        return 1;
    }
    *(uint8_t *)ros_ptr(r->start + ins) = byte;
    atomic_store_explicit(&r->ins, next, memory_order_release);
    check_filling(r, ring((int64_t)rem - 1, next, size));
    wake(r);
    return 0;
}

/* Insert up to n bytes and return how many did not fit. The source is
 * ordinary memory. Only the buffer needs to be shared. */
static uint32_t insert_block(struct record *r, const uint8_t *src, uint32_t n)
{
    if (n == 0)
        return 0;
    uint32_t flags = atomic_load(&r->flags), size = r->size, u = unit(flags);
    uint32_t ins = atomic_load_explicit(&r->ins, memory_order_relaxed);
    uint32_t rem = atomic_load_explicit(&r->rem, memory_order_acquire);
    uint32_t limit = rem >= u ? rem - u : size - u;    /* one unit short of rem */
    uint8_t *buffer = ros_ptr(r->start);
    uint32_t left = n;
    if (ins != limit) {
        /* The free space is [ins, limit), or [ins, size) and [0, limit). */
        uint32_t wraps = limit < ins;
        uint32_t k = (wraps ? size : limit) - ins;
        k = left < k ? left : k;
        memcpy(buffer + ins, src, k);
        src += k;
        left -= k;
        ins = ins + k == size ? 0 : ins + k;
        if (left && wraps && ins == 0) {
            k = left < limit ? left : limit;
            memcpy(buffer, src, k);
            src += k;
            left -= k;
            ins = k;
        }
        atomic_store_explicit(&r->ins, ins, memory_order_release);
        check_filling(r, ring(limit, ins, size));
        wake(r);
    }
    if (left && (flags & F_INPUT_FULL))
        event(EVENT_INPUT_FULL, r->handle, ros_in_arena(src) ? ros_addr(src) : 0, left);
    return left;
}

/* Remove the next byte, or with examine set only copy it. Returns 1 if
 * there is none. */
static int remove_byte(struct record *r, int examine, uint8_t *out)
{
    uint32_t size = r->size;
    uint32_t ins = atomic_load_explicit(&r->ins, memory_order_acquire);
    uint32_t rem = atomic_load_explicit(&r->rem, memory_order_relaxed);
    if (ins == rem) {
        if (!examine)
            make_dormant(r);
        return 1;
    }
    *out = *(const uint8_t *)ros_ptr(r->start + rem);
    if (examine)
        return 0;
    uint32_t next = rem + 1 == size ? 0 : rem + 1;
    atomic_store_explicit(&r->rem, next, memory_order_release);
    check_emptying(r, ring((int64_t)next - 1, ins, size));
    if (ins == next) {
        make_dormant(r);
        if (atomic_load(&r->flags) & F_OUTPUT_EMPTY)
            event(EVENT_OUTPUT_EMPTY, r->handle, 0, 0);
    }
    return 0;
}

/* Remove up to n bytes, or with examine set only copy them. Returns how
 * many could not be had. */
static uint32_t remove_block(struct record *r, int examine, uint8_t *dst, uint32_t n)
{
    if (n == 0)
        return 0;
    uint32_t size = r->size;
    uint32_t ins = atomic_load_explicit(&r->ins, memory_order_acquire);
    uint32_t rem = atomic_load_explicit(&r->rem, memory_order_relaxed);
    const uint8_t *buffer = ros_ptr(r->start);
    uint32_t left = n;
    if (ins == rem) {
        if (!examine)
            make_dormant(r);
        return left;
    }
    /* The data is [rem, ins), or [rem, size) and [0, ins). */
    uint32_t wraps = ins < rem;
    uint32_t k = (wraps ? size : ins) - rem;
    k = left < k ? left : k;
    memcpy(dst, buffer + rem, k);
    dst += k;
    left -= k;
    rem = rem + k == size ? 0 : rem + k;
    if (left && wraps && rem == 0) {
        k = left < ins ? left : ins;
        memcpy(dst, buffer, k);
        left -= k;
        rem = k;
    }
    if (examine)
        return left;
    atomic_store_explicit(&r->rem, rem, memory_order_release);
    check_emptying(r, ring((int64_t)rem - 1, ins, size));
    if (ins == rem) {
        make_dormant(r);
        if (atomic_load(&r->flags) & F_OUTPUT_EMPTY)
            event(EVENT_OUTPUT_EMPTY, r->handle, 0, 0);
    }
    return left;
}

static void purge(struct record *r)
{
    atomic_store(&r->ins, 0);
    atomic_store(&r->rem, 0);
    uint32_t flags = atomic_load(&r->flags);
    if ((flags & (F_THRESHOLD_UPCALL | F_EXCEEDED)) == (F_THRESHOLD_UPCALL | F_EXCEEDED) &&
        r->size > r->threshold)
        upcall(r, UPCALL_EMPTYING);
    make_dormant(r);
}

/* Reason 9: discard n bytes, then describe the next contiguous block.
 * A driver that sends straight from the buffer's memory wants this.
 * Returns 1 (C set) if there is no block. */
static int next_block(struct record *r, uint32_t n, uint32_t *at, uint32_t *len)
{
    uint32_t size = r->size;
    uint32_t ins = atomic_load_explicit(&r->ins, memory_order_acquire);
    uint32_t rem = atomic_load_explicit(&r->rem, memory_order_relaxed);
    uint32_t used = ring(ins, rem, size);
    uint32_t discard = n < used ? n : used;
    uint32_t left = used - discard;
    rem += discard;
    if (rem >= size)
        rem -= size;
    atomic_store_explicit(&r->rem, rem, memory_order_release);
    check_emptying(r, size - left - 1);
    *at = r->start + rem;
    *len = ins >= rem ? ins - rem : size - rem;
    if (ins == rem) {
        make_dormant(r);
        if (atomic_load(&r->flags) & F_OUTPUT_EMPTY)
            event(EVENT_OUTPUT_EMPTY, r->handle, 0, 0);
    }
    return *len == 0;
}

/* ---- the SWIs ------------------------------------------------------------ */

os_error *xbuffer_register(uint32_t flags, uint8_t *start, uint8_t *end, uint32_t handle,
                           uint32_t *handle_out)
{
    uint32_t from = ros_addr(start), to = ros_addr(end);   /* arena memory, or a trap */
    if ((flags & F_WORD_ALIGNED) && ((from | to) & 3))
        return ros_error(ERR_NOT_ALIGNED, "Buffer must be word aligned");
    if ((int32_t)(to - from) <= 0)
        return ros_error(ERR_TOO_SMALL, "Buffer too small");
    if (handle != NO_HANDLE && find(handle))
        return ros_error(ERR_HANDLE_USED, "Buffer handle already in use");

    struct workspace *w = ws();
    struct record *r = records(w);
    if (handle == NO_HANDLE) {
        /* Use the lowest number from 256 up that no buffer has. */
        handle = FIRST_HANDLE;
        for (uint32_t i = 0; i < w->count; i++)
            if (r[i].handle == handle) {
                handle++;
                i = (uint32_t)-1;
            }
    }
    struct record *slot = NULL;
    for (uint32_t i = 0; i < w->count && !slot; i++)
        if (r[i].handle == NO_HANDLE)
            slot = &r[i];
    if (!slot) {
        /* Grow the table by one. Ids are offsets into it, so they stay valid. */
        struct record *grown = ros_rma_alloc((w->count + 1) * sizeof *grown);
        if (!grown)
            return ros_error(ERR_TOO_MANY, "Too many buffers");
        if (r) {
            memcpy(grown, r, w->count * sizeof *grown);
            ros_rma_free(r);
        }
        w->table = ros_addr(grown);
        slot = &grown[w->count++];
    }
    slot->handle = handle;
    atomic_store(&slot->flags, flags & F_CLIENT);
    slot->start = from;
    slot->size = to - from;
    atomic_store(&slot->ins, 0);
    atomic_store(&slot->rem, 0);
    slot->wake_up = slot->detach = 0;
    slot->private = slot->workspace = 0;
    slot->threshold = 0;
    *handle_out = handle;
    return NULL;
}

os_error *xbuffer_create(uint32_t flags, uint32_t size, uint32_t handle, uint32_t *handle_out)
{
    if ((int32_t)size <= 0)
        return ros_error(ERR_TOO_SMALL, "Buffer too small");
    if ((flags & F_WORD_ALIGNED) && (size & 3))
        return ros_error(ERR_NOT_ALIGNED, "Buffer must be word aligned");
    uint8_t *block = ros_rma_alloc(size);
    if (!block)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    os_error *e = xbuffer_register(flags, block, block + size, handle, handle_out);
    if (e)
        ros_rma_free(block);
    return e;
}

os_error *xbuffer_deregister(uint32_t handle)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    os_error *e = call_owner(r->detach, r);
    if (e)
        return e;
    r->handle = NO_HANDLE;
    return NULL;
}

os_error *xbuffer_remove(uint32_t handle)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    uint32_t start = r->start;
    os_error *e = xbuffer_deregister(handle);
    if (e)
        return e;
    /* This is only for buffers that Buffer_Create made. It claimed their
     * memory, so it frees it. */
    return xos_module_free(ros_ptr(start));
}

os_error *xbuffer_modify_flags(uint32_t handle, uint32_t eor_mask, uint32_t and_mask,
                               uint32_t *old_flags, uint32_t *new_flags)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    uint32_t old = atomic_load(&r->flags), new;
    do
        new = (old & and_mask) ^ eor_mask;
    while (!atomic_compare_exchange_weak(&r->flags, &old, new));
    *old_flags = old;
    *new_flags = new;
    return NULL;
}

/* Linking and unlinking both empty the buffer and leave it dormant. The
 * new owner then does not receive the old owner's data. */
static void reset(struct record *r)
{
    atomic_fetch_and(&r->flags, ~(F_NOT_DORMANT | F_EXCEEDED));
    atomic_store(&r->ins, 0);
    atomic_store(&r->rem, 0);
}

os_error *xbuffer_link_device(uint32_t handle, uint32_t wake_up, uint32_t detach,
                              uint32_t private, uint32_t workspace)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    os_error *e = call_owner(r->detach, r);     /* The current owner may refuse */
    if (e)
        return e;
    r->wake_up = wake_up;
    r->detach = detach ? detach : DETACH_REFUSE;
    r->private = private;
    r->workspace = workspace;
    reset(r);
    return NULL;
}

os_error *xbuffer_unlink_device(uint32_t handle)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    r->wake_up = r->detach = 0;                  /* No warning is given */
    reset(r);
    return NULL;
}

os_error *xbuffer_get_info(uint32_t handle, uint32_t *flags, uint8_t **start, uint8_t **end,
                           uint32_t *insert, uint32_t *remove, uint32_t *free_out,
                           uint32_t *used)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    *flags = atomic_load(&r->flags);
    *start = ros_ptr(r->start);
    *end = ros_ptr(r->start + r->size);
    *insert = atomic_load(&r->ins);
    *remove = atomic_load(&r->rem);
    *free_out = free_space(r);
    *used = used_space(r);
    return NULL;
}

os_error *xbuffer_threshold(uint32_t handle, uint32_t threshold, uint32_t *previous)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    *previous = r->threshold;
    if (threshold == 0xFFFFFFFFu)
        return NULL;                             /* Read only */
    r->threshold = threshold;
    uint32_t flags = atomic_load(&r->flags);
    if (flags & F_THRESHOLD_UPCALL) {
        /* The new threshold may already have been crossed in either direction. */
        uint32_t free_now = ring((int64_t)atomic_load(&r->rem) - 1, atomic_load(&r->ins), r->size);
        if (free_now < threshold) {
            if (!(flags & F_EXCEEDED))
                upcall(r, UPCALL_FILLING);
        } else if (flags & F_EXCEEDED) {
            upcall(r, UPCALL_EMPTYING);
        }
    }
    return NULL;
}

os_error *xbuffer_internal_info(uint32_t handle, uint32_t *id, uint32_t *routine,
                                uint32_t *workspace)
{
    struct workspace *w = ws();
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    *id = (uint32_t)((uint8_t *)r - (uint8_t *)records(w));
    *routine = w->routine;
    *workspace = ros_addr(w);
    return NULL;
}

/* ---- the data operations, for C (buffers.h) ------------------------------ */

os_error *xbuffer_put(uint32_t handle, const void *data, uint32_t n, uint32_t *left)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    *left = insert_block(r, data, n);
    return NULL;
}

os_error *xbuffer_get(uint32_t handle, void *data, uint32_t n, uint32_t *left)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    *left = remove_block(r, 0, data, n);
    return NULL;
}

os_error *xbuffer_peek(uint32_t handle, void *data, uint32_t n, uint32_t *left)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    *left = remove_block(r, 1, data, n);
    return NULL;
}

os_error *xbuffer_count(uint32_t handle, uint32_t *used, uint32_t *free_out)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    if (used)
        *used = used_space(r);
    if (free_out)
        *free_out = free_space(r);
    return NULL;
}

os_error *xbuffer_purge(uint32_t handle)
{
    struct record *r = find(handle);
    if (!r)
        return bad_buffer();
    purge(r);
    return NULL;
}

/* ---- the direct-call routine --------------------------------------------- */

/* Block transfers for translated code. R2 is an arena address and R3 is a
 * count. On return R2 is past what moved, R3 is what did not move, and C is
 * set if R3 is not 0. */
static void shim_insert(struct record *r, struct ros_cpu *s)
{
    uint32_t n = s->r[3];
    uint32_t left = insert_block(r, ros_ptr(s->r[2]), n);
    s->r[2] += n - left;
    s->r[3] = left;
    s->c = left != 0;
}

static void shim_remove(struct record *r, int examine, struct ros_cpu *s)
{
    uint32_t n = s->r[3];
    uint32_t left = remove_block(r, examine, ros_ptr(s->r[2]), n);
    s->r[2] += n - left;
    s->r[3] = left;
    s->c = left != 0;
}

/* R0 is the reason. R1 is the id from Buffer_InternalInfo. R12 is the
 * workspace that Buffer_InternalInfo gave. R2 and R3 are the reason's
 * parameters. It returns as compiled code expects, with R15 equal to R14,
 * and C holds the reason's result. */
static void routine(struct ros_cpu *s)
{
    struct workspace *w = ws();
    uint32_t id = s->r[1];
    struct record *r = NULL;
    if (s->r[12] == ros_addr(w) && id % sizeof(struct record) == 0 &&
        id / sizeof(struct record) < w->count)
        r = (struct record *)((uint8_t *)records(w) + id);
    if (!r || r->handle == NO_HANDLE || s->r[0] > 9) {
        s->r[0] = ros_addr(ros_error(ERR_BAD_PARM, "Bad parameters"));
        s->v = 1;
        s->r[15] = s->r[14];
        return;
    }
    uint8_t byte;
    switch (s->r[0]) {
    case 0: s->c = insert_byte(r, (uint8_t)s->r[2]); break;
    case 1: shim_insert(r, s); break;
    case 2:
    case 4:
        s->c = remove_byte(r, s->r[0] == 4, &byte);
        if (!s->c)
            s->r[2] = byte;
        break;
    case 3:
    case 5: shim_remove(r, s->r[0] == 5, s); break;
    case 6: s->r[2] = used_space(r); break;
    case 7: s->r[2] = free_space(r); break;
    case 8: purge(r); break;
    case 9: s->c = next_block(r, s->r[3], &s->r[2], &s->r[3]); break;
    }
    s->v = 0;
    s->r[15] = s->r[14];
}

/* ---- InsV, RemV and CnpV: handle our buffers, pass the rest on ----------- */

static int insv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct record *r = find(s->r[1]);
    if (!r)
        return ROS_VECTOR_PASS;
    if (s->r[1] & BLOCK_BIT) {
        shim_insert(r, s);
    } else {
        s->r[2] = s->r[0];                       /* R2 is corrupted, and holds the byte */
        s->c = insert_byte(r, (uint8_t)s->r[0]);
    }
    return ROS_VECTOR_CLAIM;
}

/* V set on entry means examine the byte instead of removing it. */
static int remv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct record *r = find(s->r[1]);
    if (!r)
        return ROS_VECTOR_PASS;
    if (s->r[1] & BLOCK_BIT) {
        shim_remove(r, s->v, s);
    } else {
        uint8_t byte;
        s->c = remove_byte(r, s->v, &byte);
        if (!s->c)
            s->r[0] = s->r[2] = byte;            /* In both, for OS_Byte's callers */
    }
    return ROS_VECTOR_CLAIM;
}

/* V set means purge. Otherwise C clear counts the bytes in the buffer and
 * C set counts the free space. The count is returned split as OS_Byte 128
 * splits it, with the low 8 bits in R1 and the rest in R2. */
static int cnpv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    struct record *r = find(s->r[1]);
    if (!r)
        return ROS_VECTOR_PASS;
    if (s->v) {
        purge(r);
        s->v = 0;
        return ROS_VECTOR_CLAIM;
    }
    uint32_t n = s->c ? free_space(r) : used_space(r);
    s->r[1] = n & 0xFF;
    s->r[2] = n >> 8;
    return ROS_VECTOR_CLAIM;
}

/* ---- the module ---------------------------------------------------------- */

static void starting(void *arg)
{
    (void)arg;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = SERVICE_BUFFER_STARTING;           /* All SWIs are valid now */
    ros_service_call(&s);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct workspace *w = ros_rma_alloc(sizeof *w);
    if (!w)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memset(w, 0, sizeof *w);
    w->routine = ros_native_entry(routine, "BufferManager:ServiceRoutine");
    ros_st32(m->private_word, ros_addr(w));
    os_error *e;
    if ((e = ros_vector_claim_native(ROS_INSV, insv, 0)) ||
        (e = ros_vector_claim_native(ROS_REMV, remv, 0)) ||
        (e = ros_vector_claim_native(ROS_CNPV, cnpv, 0)))
        return e;
    ros_callback_add_native(starting, NULL);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct workspace *w = ws();
    struct record *r = records(w);
    /* Every owner must agree to let go. If one does not, the manager stays. */
    for (uint32_t i = 0; i < w->count; i++) {
        if (r[i].handle == NO_HANDLE)
            continue;
        if (call_owner(r[i].detach, &r[i]))
            return ros_error(ERR_IN_USE, "Buffer manager in use");
        r[i].handle = NO_HANDLE;
    }
    ros_vector_release_native(ROS_INSV, insv, 0);
    ros_vector_release_native(ROS_REMV, remv, 0);
    ros_vector_release_native(ROS_CNPV, cnpv, 0);
    if (r)
        ros_rma_free(r);
    ros_rma_free(w);
    ros_st32(m->private_word, 0);
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)offset;
    return ros_error(ERR_BAD_SWI, "SWI value out of range for module %s", m->title);
}

struct ros_module buffer_manager_module = {
    .title = "BufferManager",
    .help = "Buffer Manager\t0.39 (24 Sep 2026) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x42940,
    .swi_thunks = ros_swi_thunks_BufferManager,
    .swi_names = ros_swi_names_BufferManager,
    .swi_prefix = "Buffer",
};

/* The SWI count is only known at run time (api_gen.c). The runtime reads
 * it when the module is added. */
__attribute__((constructor)) static void count(void)
{
    buffer_manager_module.swi_count = ros_swi_count_BufferManager;
}
