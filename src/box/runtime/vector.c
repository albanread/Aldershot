/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* vector.c -- software vectors, events and callbacks.
 *
 * The chains are in the runtime's own memory. Every task runs as a thread
 * of the one process, so all tasks see the same set of chains.
 */
#include <stdlib.h>

#include "rosgd/api.h"
#include "rosgd/armbox.h"
#include "rosgd/background.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/switrace.h"
#include "rosgd/vector.h"

struct node {
    struct node *next;
    uint32_t code;              /* a compiled claimant, or 0 */
    ros_vector_fn *fn;          /* a claimant in C, or NULL */
    uint32_t r12;
    uint8_t arm;                /* ARM code: perhaps a shadow's, asked when called */
};

static struct node *chains[ROS_VECTORS];

#define ERR_BAD_CLAIM_NUM  0x1A0u    /* "Bad vector number" (hdr/NewErrors) */
#define ERR_NAFF_RELEASE   0x1A1u    /* "Bad vector release" */

/* ARM shadows' claims. A claim whose code lies in a shadow's image is the
 * shadow's. The address says whose it is, so no caller kind is needed.
 * Whether a claim is a shadow's is asked when the vector is called. So a
 * shadow that is promoted to serve everyone has its claims act for
 * everyone. An ARM-only module that is demoted to a shadow has its claims
 * scoped from then on.
 *   - Service vectors (all but the three below) call a shadow's claimant
 *     only for an ARM caller (ros_caller_kind()). For any other caller it
 *     is stepped over, as if it had passed the call on. The exception is
 *     when *ARMPrefer prefers the shadow. Then it acts for every caller.
 *   - TickerV, EventV and UpCallV are notifications that are mostly issued
 *     in the background. They call shadows' claimants for every caller, but
 *     after every native claimant. A shadow's claim ends only the shadows'
 *     part.
 *   - GraphicsV is refused to a shadow, because the display driver is the
 *     box's. */
static int shadows_last(uint32_t vector)
{
    return vector == ROS_TICKERV || vector == ROS_EVENTV || vector == ROS_UPCALLV;
}

static struct ros_module *shadow_claim(const struct node *n)
{
    return n->arm ? ros_module_shadow_at(n->code) : NULL;
}

/* Whether a shadow is preferred by *ARMPrefer. Its claims on the service
 * vectors then act for every caller. */
static int preferred(const struct ros_module *sh)
{
    return sh->twin && sh->twin->prefer_arm;
}

static os_error *add(uint32_t vector, uint32_t code, ros_vector_fn *fn, uint32_t r12, int replace)
{
    if (vector >= ROS_VECTORS)
        return ros_error(ERR_BAD_CLAIM_NUM, "Bad vector number");
    int arm = code && ros_armrun_is_code(code);
    if (arm && vector == ROS_GRAPHICSV && ros_module_shadow_at(code))
        return ros_error(ERR_BAD_CLAIM_NUM, "Bad vector number");
    if (replace) {
        for (struct node **p = &chains[vector]; *p;) {
            if ((*p)->code == code && (*p)->fn == fn && (*p)->r12 == r12) {
                struct node *gone = *p;
                *p = gone->next;
                free(gone);
            } else {
                p = &(*p)->next;
            }
        }
    }
    struct node *n = malloc(sizeof *n);
    if (!n)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for a vector claim");
    *n = (struct node){ chains[vector], code, fn, r12, (uint8_t)arm };
    chains[vector] = n;
    return NULL;
}

static os_error *release(uint32_t vector, uint32_t code, ros_vector_fn *fn, uint32_t r12)
{
    if (vector >= ROS_VECTORS)
        return ros_error(ERR_BAD_CLAIM_NUM, "Bad vector number");
    for (struct node **p = &chains[vector]; *p; p = &(*p)->next) {
        if ((*p)->code == code && (*p)->fn == fn && (*p)->r12 == r12) {
            struct node *gone = *p;
            *p = gone->next;
            free(gone);
            return NULL;
        }
    }
    return ros_error(ERR_NAFF_RELEASE, "Bad vector release");
}

os_error *ros_vector_claim(uint32_t vector, uint32_t code, uint32_t r12, int add_only)
{
    return add(vector, code, NULL, r12, !add_only);
}

os_error *ros_vector_release(uint32_t vector, uint32_t code, uint32_t r12)
{
    return release(vector, code, NULL, r12);
}

os_error *ros_vector_claim_native(uint32_t vector, ros_vector_fn *fn, uint32_t r12)
{
    return add(vector, 0, fn, r12, 1);
}

os_error *ros_vector_release_native(uint32_t vector, ros_vector_fn *fn, uint32_t r12)
{
    return release(vector, 0, fn, r12);
}

/* Calls one claimant. It returns 1 if the claimant claimed the call. */
static int call_claimant(const struct node *n, struct ros_cpu *s)
{
    if (n->fn)
        return n->fn(s, n->r12) == ROS_VECTOR_CLAIM;
    s->r[12] = n->r12;
    s->r[14] = ROS_VECTOR_PASSED;
    ros_call(s, n->code);
    ros_continue(s);            /* a return elsewhere is a jump there */
    if (s->r[15] == ROS_VECTOR_CLAIMED)
        return 1;               /* it pulled the claim address */
    if (s->r[15] != ROS_VECTOR_PASSED)
        ros_bad_return(s, ROS_VECTOR_PASSED);
    return 0;
}

int ros_vector_call(uint32_t vector, struct ros_cpu *s)
{
    if (vector >= ROS_VECTORS)
        return 0;
    /* The claim address goes on the stack once, for the whole chain, as
     * the kernel's CallVector pushes the caller's return. It goes on the
     * SVC stack, where a vector always runs. That is below the caller's
     * frames if the caller is on the SVC stack. Otherwise, for an
     * application's SWI on the application's own stack, it is where the SVC
     * stack stands. Suppose it were built on the caller's stack. A vector's
     * chain, and all it calls (a mode change's service calls among them),
     * would run on BASIC's stack. The next vector down would write its
     * claim address over the frames of the code between. This happened with
     * MODE in BASIC: the Wimp's Service_ModeChange came back to
     * &FFFFFFF0. */
    uint32_t sp = s->r[13], lr = s->r[14];
    uint32_t base = sp - ROS_SVCSTACK_BASE <= ROS_SVCSTACK_SIZE && sp < ros_svc_sp ? sp : ros_svc_sp;
    s->r[13] = base - 4;
    ros_st32(s->r[13], ROS_VECTOR_CLAIMED);
    uint32_t outer_sp = ros_svc_sp_enter(s);      /* what C claimants call builds below it */

    int claimed = 0, deferred = 0, kind = -1;
    for (struct node *n = chains[vector], *next; n && !claimed; n = next) {
        next = n->next;         /* a claimant may release itself */
        struct ros_module *sh = n->arm ? shadow_claim(n) : NULL;
        if (sh) {
            if (shadows_last(vector)) {
                deferred = 1;   /* it is called after every native claimant */
                continue;
            }
            if (kind < 0)
                kind = ros_caller_kind();
            if (kind != ROS_KIND_ARM && !preferred(sh))
                continue;       /* for a native caller, as if it passed the call on */
        }
        claimed = call_claimant(n, s);
    }
    for (struct node *n = deferred ? chains[vector] : NULL, *next; n && !claimed; n = next) {
        next = n->next;
        if (n->arm && shadow_claim(n))
            claimed = call_claimant(n, s);
    }
    ros_svc_sp = outer_sp;
    s->r[13] = sp;
    s->r[14] = lr;
    return claimed;
}

/* ---- events ------------------------------------------------------------ */

/* A byte for each event, as the kernel keeps them (OS_Byte 13 and 14).
 * Enabling counts up and stops at &FF, which then stays. Disabling counts
 * down, so disabling an event that was not enabled leaves it at &FF. */
static uint8_t semaphores[32];

uint32_t ros_event_semaphore(uint32_t event, int enable)
{
    if (event >= 32)
        return 0xFF;                    /* "was enabled", and nothing changes */
    uint8_t was = semaphores[event];
    if (was != 0xFF)
        semaphores[event] = (uint8_t)(enable ? was + 1 : was - 1);
    return was;
}

void ros_event_enable(uint32_t event, int on)
{
    if (event < 32 && (on || semaphores[event]))
        ros_event_semaphore(event, on);
}

void ros_event_generate(struct ros_cpu *s)
{
    uint32_t event = s->r[0];
    if (event < 32 && !semaphores[event]) {
        s->c = 1;
        return;
    }
    ros_vector_call(ROS_EVENTV, s);
    s->c = 0;
}

/* ---- callbacks --------------------------------------------------------- */

struct callback {
    struct callback *next;
    uint32_t code, r12;
    void (*fn)(void *arg);
    void *arg;
};

static struct callback *pending, **pending_tail = &pending;

static os_error *queue(uint32_t code, uint32_t r12, void (*fn)(void *), void *arg)
{
    struct callback *c = malloc(sizeof *c);
    if (!c)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room for a callback");
    *c = (struct callback){ NULL, code, r12, fn, arg };
    *pending_tail = c;
    pending_tail = &c->next;
    return NULL;
}

os_error *ros_callback_add(uint32_t code, uint32_t r12)
{
    return queue(code, r12, NULL, NULL);
}

void ros_callback_add_native(void (*fn)(void *), void *arg)
{
    queue(0, 0, fn, arg);
}

os_error *ros_callback_remove(uint32_t code, uint32_t r12)
{
    for (struct callback **p = &pending; *p; p = &(*p)->next) {
        if ((*p)->code == code && (*p)->r12 == r12 && !(*p)->fn) {
            struct callback *gone = *p;
            *p = gone->next;
            if (pending_tail == &gone->next)
                pending_tail = p;
            free(gone);
            break;
        }
    }
    return NULL;
}

void ros_callbacks_run(void)
{
    while (pending) {
        struct callback *c = pending;
        pending = c->next;
        if (!pending)
            pending_tail = &pending;
        if (c->fn) {
            c->fn(c->arg);
        } else {
            /* A callback has no caller to return an error to, so an error
             * is reported. Compiled code runs inside the OS, as a module's
             * entry points do (module.c). The SWIs it makes are not the
             * outermost, so background work waits until it returns. At
             * depth 0 background work would run on the SVC stack's top,
             * over the callback's own frame. This happened with the Filter
             * Manager's callback, which issues a service call and came
             * back to a vector's return address. */
            struct ros_handler h;
            struct ros_cpu s;
            uint32_t depth = ros_call_depth;
            if (ROS_TRY(&h)) {
                ros_cpu_enter(&s);
                s.r[12] = c->r12;
                ros_call_depth = depth + 1;
                ros_call(&s, c->code);
                ros_call_depth = depth;
                if (s.r[15] != ROS_RETURN_TO_NATIVE)
                    ros_bad_return(&s, ROS_RETURN_TO_NATIVE);
                ros_handler_pop(&h);
            } else {
                ros_call_depth = depth;
                ros_console_printf("rosgd: callback &%08X: %s\n", c->code, h.error->errmess);
            }
        }
        free(c);
    }
}

/* ---- the SWIs whose registers pass straight through --------------------- */

void ros_thunk_OS_CallAVector(struct ros_cpu *s)
{
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    ros_vector_call(s->r[9], s);
    ros_svc_sp = outer_sp;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    /* V is an input to RemV and CnpV, not a result. Clear it, as the SWI's
     * success. */
    s->v = 0;
}

void ros_thunk_OS_GenerateEvent(struct ros_cpu *s)
{
    /* R10-R12 come back as they went, as from any SWI. A compiled claimant
     * is entered with its own R12 (the Screen Blanker's). */
    uint32_t r10 = s->r[10], r11 = s->r[11], r12 = s->r[12];
    uint32_t outer_sp = ros_svc_sp_enter(s);
    ros_event_generate(s);
    ros_svc_sp = outer_sp;
    s->r[10] = r10;
    s->r[11] = r11;
    s->r[12] = r12;
    s->v = 0;
}

/* ---- the typed ones ----------------------------------------------------- */

os_error *xos_claim(uint32_t vector, uint32_t code, uint32_t r12)
{
    return ros_vector_claim(vector, code, r12, 0);
}

os_error *xos_release(uint32_t vector, uint32_t code, uint32_t r12)
{
    return ros_vector_release(vector, code, r12);
}

os_error *xos_add_to_vector(uint32_t vector, uint32_t code, uint32_t r12)
{
    return ros_vector_claim(vector, code, r12, 1);
}

os_error *xos_add_call_back(uint32_t code, uint32_t r12)
{
    return ros_callback_add(code, r12);
}

os_error *xos_remove_call_back(uint32_t code, uint32_t r12)
{
    return ros_callback_remove(code, r12);
}
