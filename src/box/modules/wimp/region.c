/* region.c: rectangle lists.
 *
 * A list is an ordered sequence of disjoint rectangles in screen
 * coordinates, with x1 and y1 exclusive.  The operations follow RISC OS
 * 5.30's exactly, down to the order in which pieces are appended and
 * merged.  This is so that the rectangles a task is handed come in 5.30's
 * sequence and shapes.  Each operation builds a new list and then
 * replaces its destination with it.
 *
 * Lists are fixed arrays.  The desktop's own lists (the invalid region and
 * the pending redraw list) are in the workspace.  Working lists are on the
 * heap for the length of one call.  A list that would overflow is marked,
 * and the Wimp recovers by making the whole screen invalid. */
#include <stdlib.h>
#include <string.h>

#include "wimp.h"

static int empty(const struct wimp_box *b)
{
    return b->x0 >= b->x1 || b->y0 >= b->y1;
}

struct wimp_rlist *wimp_rl_new(void)
{
    struct wimp_rlist *l = malloc(sizeof *l);
    if (l) {
        l->n = 0;
        l->overflow = 0;
    }
    return l;
}

void wimp_rl_free(struct wimp_rlist *l)
{
    free(l);
}

void wimp_rl_copy(struct wimp_rlist *to, const struct wimp_rlist *from)
{
    to->n = from->n;
    to->overflow = from->overflow;
    memcpy(to->b, from->b, from->n * sizeof from->b[0]);
}

/* append(R, r): the rectangle is clipped to the screen.  If it extends an
 * element exactly, it is merged with that element and the search starts
 * again.  Otherwise it is added at the end. */
void wimp_rl_append(struct wimp_rlist *l, struct wimp_box r)
{
    struct wimp_box s = wimp_screen_box();
    if (r.x0 < s.x0) r.x0 = s.x0;
    if (r.y0 < s.y0) r.y0 = s.y0;
    if (r.x1 > s.x1) r.x1 = s.x1;
    if (r.y1 > s.y1) r.y1 = s.y1;
    if (empty(&r))
        return;
    for (uint32_t i = 0; i < l->n;) {
        struct wimp_box *e = &l->b[i];
        int merge = 0;
        if (e->x0 == r.x0 && e->x1 == r.x1 && (r.y0 == e->y1 || r.y1 == e->y0))
            merge = 1;
        else if (e->y0 == r.y0 && e->y1 == r.y1 && (r.x0 == e->x1 || r.x1 == e->x0))
            merge = 1;
        if (!merge) {
            i++;
            continue;
        }
        if (e->x0 < r.x0) r.x0 = e->x0;
        if (e->y0 < r.y0) r.y0 = e->y0;
        if (e->x1 > r.x1) r.x1 = e->x1;
        if (e->y1 > r.y1) r.y1 = e->y1;
        memmove(e, e + 1, (l->n - i - 1) * sizeof *e);
        l->n--;
        i = 0;
    }
    if (l->n == WIMP_RL_MAX) {
        l->overflow = 1;
        return;
    }
    l->b[l->n++] = r;
}

static struct wimp_box meet(struct wimp_box a, struct wimp_box b)
{
    struct wimp_box r = {
        a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
        a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
    };
    return r;
}

static int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

/* intersect(L, c) */
void wimp_rl_clip(struct wimp_rlist *l, struct wimp_box c)
{
    struct wimp_rlist *r = wimp_rl_new();
    if (!r)
        return;
    c = wimp_round_box(c);
    for (uint32_t i = 0; i < l->n; i++)
        wimp_rl_append(r, meet(l->b[i], c));
    r->overflow |= l->overflow;
    wimp_rl_copy(l, r);
    wimp_rl_free(r);
}

/* subtract(L, c): each element is cut into four pieces.  These are below,
 * above, left of and right of c, in that order. */
void wimp_rl_subtract(struct wimp_rlist *l, struct wimp_box c)
{
    struct wimp_rlist *r = wimp_rl_new();
    if (!r)
        return;
    c = wimp_round_box(c);
    for (uint32_t i = 0; i < l->n; i++) {
        struct wimp_box e = l->b[i];
        wimp_rl_append(r, (struct wimp_box){ e.x0, min32(e.y0, c.y0), e.x1, min32(e.y1, c.y0) });
        wimp_rl_append(r, (struct wimp_box){ e.x0, max32(e.y0, c.y1), e.x1, max32(e.y1, c.y1) });
        int32_t y0 = max32(e.y0, c.y0), y1 = min32(e.y1, c.y1);
        wimp_rl_append(r, (struct wimp_box){ min32(e.x0, c.x0), y0, min32(e.x1, c.x0), y1 });
        wimp_rl_append(r, (struct wimp_box){ max32(e.x0, c.x1), y0, max32(e.x1, c.x1), y1 });
    }
    r->overflow |= l->overflow;
    wimp_rl_copy(l, r);
    wimp_rl_free(r);
}

/* add(L, c): subtract, then append */
void wimp_rl_add(struct wimp_rlist *l, struct wimp_box c)
{
    wimp_rl_subtract(l, c);
    wimp_rl_append(l, wimp_round_box(c));
}

/* L1 - L2 */
void wimp_rl_minus(struct wimp_rlist *l, const struct wimp_rlist *m)
{
    for (uint32_t i = 0; i < m->n; i++)
        wimp_rl_subtract(l, m->b[i]);
}

/* L1 ∩ L2: the outer loop is over L2 and the inner loop over L1. */
void wimp_rl_and(struct wimp_rlist *l, const struct wimp_rlist *m)
{
    struct wimp_rlist *r = wimp_rl_new();
    if (!r)
        return;
    for (uint32_t j = 0; j < m->n; j++)
        for (uint32_t i = 0; i < l->n; i++)
            wimp_rl_append(r, meet(l->b[i], m->b[j]));
    r->overflow |= l->overflow | m->overflow;
    wimp_rl_copy(l, r);
    wimp_rl_free(r);
}

/* L1 ∪ L2 */
void wimp_rl_union(struct wimp_rlist *l, const struct wimp_rlist *m)
{
    for (uint32_t i = 0; i < m->n; i++)
        wimp_rl_add(l, m->b[i]);
    l->overflow |= m->overflow;
}

void wimp_rl_translate(struct wimp_rlist *l, int32_t dx, int32_t dy)
{
    for (uint32_t i = 0; i < l->n; i++) {
        l->b[i].x0 += dx, l->b[i].x1 += dx;
        l->b[i].y0 += dy, l->b[i].y1 += dy;
    }
}

int wimp_rl_meets(const struct wimp_rlist *l, struct wimp_box c)
{
    for (uint32_t i = 0; i < l->n; i++) {
        struct wimp_box m = meet(l->b[i], c);
        if (!empty(&m))
            return 1;
    }
    return 0;
}
