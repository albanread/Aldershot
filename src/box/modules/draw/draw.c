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
 * This file is a reimplementation in C of RISC OS Open's ARM assembler source
 * (Sources/Video/Render/DrawMod: s.Draw, hdr.Draw).
 */

/* draw.c: the Draw module (Video/Render/DrawMod), reimplemented. It holds
 * the module, DrawV, the SWIs, and Draw_ProcessPath itself.
 *
 * As in RISC OS 5, every SWI goes through DrawV, whose default owner is
 * this module. Draw_Fill, Draw_Stroke and the rest are each turned into a
 * Draw_ProcessPath call, which goes through DrawV again. So a claimant (a
 * printer driver) sees them all as it does in RISC OS 5. ProcessPath builds
 * its chain of stages (process.c and output.c) and passes the path down it
 * one element at a time. The code is s/Draw's, transliterated. The
 * workspace is draw_ws. There is one for the whole box, as the module has
 * one.
 *
 * The *FP SWIs behave as the original's do. The floating point stage is
 * "not in this version of Draw", and Draw_ProcessPathFP is unimplemented.
 */
#include <stdlib.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/module.h"
#include "rosgd/swi.h"
#include "rosgd/vdu.h"
#include "rosgd/vector.h"
#include "draw.h"

#define DRAWV 0x20

struct draw draw_ws;

/* ---- errors (Resources:$.Resources.DrawMod.Messages, UK) -------------------------------- */

os_error *draw_err(uint32_t n)
{
    static const char *const text[] = {
        "Draw module does not work in IRQ mode", "Bad Draw_ProcessPath reason code",
        "Reserved bits not zero", "Invalid address", "Bad path element",
        "Path elements out of order", "Operation may change path length", "Output path full",
        "Path needs to be flattened", "Invalid cap and join specification",
        "Overflow while transforming point", "Draw can only plot to graphics modes",
    };
    if (n == DE_UNIMPLEMENTED)
        return ros_error(n, "Facility not in this version of Draw");
    return ros_error(n, "%s", text[n - DE_IRQ]);
}

static uint32_t draw_swi(uint32_t n, uint32_t r[10], os_error **e)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n);
    memcpy(r, c.r, 10 * sizeof r[0]);
    *e = c.v ? ros_ptr(c.r[0]) : NULL;
    return c.c;
}

/* ---- ProcessPath -------------------------------------------------------------------------- */

/* restorecursors: after a fill, restore the cursors and update the OS's
 * ChangedBox. */
static void restorecursors(struct draw *d)
{
    if (d->outputtype != SPEC_FILL && d->outputtype != SPEC_FILLBYSUBPATHS)
        return;
    uint32_t r[10] = { 0 };
    os_error *e;
    draw_swi(XOS_RestoreCursors, r, &e);
    if (ros_ld32(d->changedboxaddr) & 1)
        draw_updatechangedbox(d);
}

static os_error *chain(struct draw *d, struct draw_el *e)
{
    return d->chain[0](d, 0, e);
}

/* closeopensubpath: close an open subpath with a line back to its start. */
static os_error *closeopen(struct draw *d, struct draw_el *e)
{
    if (!(d->flags & F_CLOSEOPEN) || d->subpathtype != ET_MOVE)
        return NULL;
    e->type = ET_CLOSELINE;
    e->x = d->subpathstart[0], e->y = d->subpathstart[1];
    return chain(d, e);
}

/* The element loop, run once the chain is built. R0 is returned through r0. */
static os_error *run_path(struct draw *d, uint32_t *r0)
{
    struct draw_el e;
    memset(&e, 0, sizeof e);
    e.x0 = (int32_t)d->inputptr, e.y0 = (int32_t)d->flags;
    e.x = (int32_t)d->dashptr, e.y = (int32_t)d->outputtype;
    e.type = ET_START;
    d->subpathtype = ET_END;
    os_error *err = chain(d, &e);
    while (!err) {
        uint32_t q = d->inputptr, type = ros_ld8(q);
        q += 4;
        if (type > ET_LINE) {
            err = draw_err(DE_ELEMENT);
            break;
        }
        switch (type) {
        case ET_END:
            if ((err = closeopen(d, &e)))
                break;
            e.type = ET_END;
            err = chain(d, &e);
            restorecursors(d);
            *r0 = (uint32_t)e.x0;
            return err;
        case ET_CONTINUE:
            d->inputptr = ros_ld32(q);
            continue;
        case ET_MOVE: case ET_SPECIALMOVE:
            if ((err = closeopen(d, &e)))
                break;
            q = d->inputptr + 4;
            e.type = ros_ld8(d->inputptr);
            e.x = (int32_t)ros_ld32(q), e.y = (int32_t)ros_ld32(q + 4);
            d->inputptr = q + 8;
            d->subpathstart[0] = e.x, d->subpathstart[1] = e.y;
            d->subpathtype = e.type;
            err = chain(d, &e);
            continue;
        case ET_CLOSEGAP: case ET_CLOSELINE:
            d->inputptr = q;
            if (d->subpathtype == ET_END) {
                err = draw_err(DE_SEQUENCE);
                break;
            }
            e.type = type;
            e.x = d->subpathstart[0], e.y = d->subpathstart[1];
            d->subpathtype = ET_END;
            err = chain(d, &e);
            continue;
        case ET_BEZIER:
            e.c1x = (int32_t)ros_ld32(q), e.c1y = (int32_t)ros_ld32(q + 4);
            e.c2x = (int32_t)ros_ld32(q + 8), e.c2y = (int32_t)ros_ld32(q + 12);
            q += 16;
            /* fall through */
        default:
            e.x = (int32_t)ros_ld32(q), e.y = (int32_t)ros_ld32(q + 4);
            d->inputptr = q + 8;
            if (d->subpathtype == ET_END) {
                err = draw_err(DE_SEQUENCE);
                break;
            }
            e.type = type;
            err = chain(d, &e);
            continue;
        }
        break;
    }
    restorecursors(d);
    return err;
}

/* The default Draw_ProcessPath (processpath). R0-R7 come in and R0 goes out. */
os_error *draw_process_path(struct draw *d, const uint32_t r[8], uint32_t *r0)
{
    d->inputptr = r[0], d->flags = r[1], d->matrix = r[2];
    d->flatness = (int32_t)r[3], d->thickness = (int32_t)r[4];
    d->joinsandcaps = r[5], d->dashptr = r[6], d->outputtype = r[7];
    uint32_t flags = r[1], r7 = r[7];
    if ((flags & ~(uint32_t)F_STYLEMASK) & ~F_FLAGSMASK)
        return draw_err(DE_RESERVED);
    if ((r[0] | r[2] | r[6]) & 3)
        return draw_err(DE_ADDRESS);
    draw_stage *rev[8];
    int n = 0;
    int bbox = 0;
    if (flags & F_R7IS32BIT) {
        bbox = (flags & F_R7ISBBOX) != 0;
    } else {
        if (flags & F_R7ISBBOX)
            return draw_err(DE_RESERVED);
        if (r7 & 0x80000000u)
            r7 &= 0x7FFFFFFF, bbox = 1;
    }
    if (bbox) {
        if (r7 & 3)
            return draw_err(DE_ADDRESS);
        rev[n++] = out_box;
    } else if (r7 == SPEC_INSITU) {
        if (((flags & F_THICKEN) && r[4]) || (flags & (F_CLOSEOPEN | F_FLATTEN | F_REFLATTEN)) ||
            r[6])
            return draw_err(DE_MAYEXPAND);
        rev[n++] = out_replace;
    } else if (r7 == SPEC_COUNT) {
        rev[n++] = out_count;
    } else if (r7 == SPEC_FILL || r7 == SPEC_FILLBYSUBPATHS) {
        uint32_t c[10] = { 0xFFFFFFFFu };
        os_error *e;
        draw_swi(XOS_ChangedBox, c, &e);
        if (e)
            return e;
        d->changedboxaddr = c[1];
        if (c[0] & 1)
            draw_initbox(d);
        rev[n++] = r7 == SPEC_FILL ? out_fill : out_subpaths;
        uint32_t blk = ros_vdu_scratch();
        static const uint32_t vars[] = { 128, 129, 130, 131, 136, 137, 4, 5, 0xAB, 0, 0xFFFFFFFF };
        for (unsigned k = 0; k < sizeof vars / sizeof vars[0]; k++)
            ros_st32(blk + 4 * k, vars[k]);
        uint32_t v[10] = { blk, blk + 64 };
        draw_swi(XOS_ReadVduVariables, v, &e);
        if (!e) {
            uint32_t z[10] = { 0 };
            draw_swi(XOS_RemoveCursors, z, &e);
        }
        if (e)
            return e;
        int32_t *w[] = { &d->lcol, &d->brow, &d->rcol, &d->trow, &d->orgx, &d->orgy, &d->xeig,
                         &d->yeig };
        for (unsigned k = 0; k < 8; k++)
            *w[k] = (int32_t)ros_ld32(blk + 64 + 4 * k);
        d->modeflags = ros_ld32(blk + 64 + 36);
        if (d->modeflags & 1) {
            restorecursors(d);
            return draw_err(DE_GRAPHICS);
        }
        rev[n++] = process_longedgeprotect;
    } else {
        if (r7 & 3)
            return draw_err(DE_ADDRESS);
        rev[n++] = out_path;
    }
    if (flags & F_FLOAT)
        rev[n++] = process_float;
    if (r[2])
        rev[n++] = process_transform;
    if (flags & F_REFLATTEN)
        rev[n++] = process_flatten;
    if (flags & F_THICKEN) {
        if (r[4] && (r[5] & 3)) {
            restorecursors(d);
            return draw_err(DE_ADDRESS);
        }
        rev[n++] = r[4] ? process_thicken : process_zerothicken;
    }
    if (r[6])
        rev[n++] = process_dash;
    if (flags & F_FLATTEN)
        rev[n++] = process_flatten;
    d->nchain = n;
    for (int k = 0; k < n; k++)
        d->chain[k] = rev[n - 1 - k];
    d->userspacewinding = 1;
    return run_path(d, r0);
}

/* ---- the reasons: each becomes a ProcessPath, through DrawV again ------------------------ */

/* changetoprocesspath: call DrawV again with R8 reduced to ProcessPath or
 * ProcessPathFP (bit 0 of R8 keeps the FP form). */
static void via_drawv(struct ros_cpu *s, const uint32_t r[8])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 8 * sizeof r[0]);
    c.r[8] = s->r[8] & 1, c.r[9] = DRAWV;
    c.v = 0;
    ros_vector_call(DRAWV, &c);
    s->r[0] = c.r[0], s->v = c.v;
}

static os_error *style(uint32_t *r1, uint32_t def)
{
    if (*r1 == 0)
        *r1 = def;
    if (*r1 & ~(uint32_t)F_STYLEMASK)
        return draw_err(DE_RESERVED);
    return NULL;
}

static void fail(struct ros_cpu *s, os_error *e)
{
    ros_swi_fail(s, e);
}

/* The registers for a ProcessPath call that does a stroke, made from R0-R7.
 * fp is set for the FP form. */
static os_error *stroke_regs(uint32_t r[8], int fp, uint32_t extra)
{
    int zero = (fp ? r[4] << 1 : r[4]) == 0;
    uint32_t r9 = F_FLATTEN | F_THICKEN | (zero ? 0 : F_REFLATTEN);
    r[7] = (int32_t)r[1] >= 0 ? SPEC_FILLBYSUBPATHS : SPEC_FILL;
    r[1] &= 0x7FFFFFFF;
    os_error *e = style(&r[1], zero ? F_INTERIORBDRY | F_EXTERIORBDRY : F_FULLINTERIOR | F_INTERIORBDRY);
    if (e)
        return e;
    r[1] |= r9 | extra;
    return NULL;
}

/* Reasons 2-11 turned into ProcessPath's registers. R0-R7 are changed in
 * place. */
os_error *draw_reason_regs(uint32_t reason, uint32_t r[8])
{
    int fp = reason & 1, zero;
    os_error *e;
    switch (reason & ~1u) {
    case 2:                                     /* Fill */
        if ((e = style(&r[1], F_FULLINTERIOR | F_INTERIORBDRY)))
            return e;
        r[1] |= F_CLOSEOPEN | F_FLATTEN;
        r[6] = 0, r[7] = SPEC_FILL;
        return NULL;
    case 4:                                     /* Stroke */
        return stroke_regs(r, fp, 0);
    case 6:                                     /* StrokePath */
        if (r[1] & 3)
            return draw_err(DE_ADDRESS);
        r[7] = r[1] ? r[1] : SPEC_COUNT;
        zero = (fp ? r[4] << 1 : r[4]) == 0;
        r[1] = F_FLATTEN | F_THICKEN | (zero ? 0 : F_REFLATTEN);
        return NULL;
    case 8:                                     /* FlattenPath */
        if (r[1] & 3)
            return draw_err(DE_ADDRESS);
        r[7] = r[1] ? r[1] : SPEC_COUNT;
        r[3] = r[2];
        r[1] = F_FLATTEN | (fp ? F_FLOAT : 0);
        r[2] = 0, r[6] = 0;
        return NULL;
    case 10:                                    /* TransformPath */
        if (r[1] & 3)
            return draw_err(DE_ADDRESS);
        r[7] = r[1];
        if (r[3] & ~F_FLOAT)
            return draw_err(DE_RESERVED);
        r[1] = r[3], r[6] = 0;
        return NULL;
    default:
        return draw_err(DE_REASON);
    }
}

/* Fill, Stroke, StrokePath, FlattenPath and TransformPath each become a
 * ProcessPath call. */
static void reason(struct ros_cpu *s)
{
    uint32_t r[8];
    memcpy(r, s->r, sizeof r);
    os_error *e = draw_reason_regs(s->r[8], r);
    if (e) {
        fail(s, e);
        return;
    }
    via_drawv(s, r);
}

/* Run the clipping path, keeping its spans (F_BUFFER), then sort them. Its
 * errors are not reported, as the original's are not. */
static void clip_path(struct ros_cpu *s, uint32_t blk, uint32_t flatness)
{
    struct draw *d = &draw_ws;
    uint32_t r[8] = { ros_ld32(blk), ros_ld32(blk + 4), ros_ld32(blk + 8), flatness,
                      s->r[4], s->r[5], 0, SPEC_FILL };
    if (r[1] == 0)
        r[1] = F_FULLINTERIOR | F_INTERIORBDRY;
    d->clipused = d->clipcount = 0;
    r[1] |= F_CLOSEOPEN | F_FLATTEN | F_BUFFER;
    uint32_t keep = s->r[0], v = s->v;
    via_drawv(s, r);
    s->r[0] = keep, s->v = v;
    draw_clip_sort(d);
}

static void fillclipped(struct ros_cpu *s)
{
    uint32_t r[8];
    memcpy(r, s->r, sizeof r);
    uint32_t cs = ros_ld32(s->r[4] + 4);
    os_error *e = style(&r[1], F_FULLINTERIOR | F_INTERIORBDRY);
    if (!e)
        e = style(&cs, F_FULLINTERIOR | F_INTERIORBDRY);
    if (e) {
        fail(s, e);
        return;
    }
    clip_path(s, s->r[4], s->r[3]);
    r[1] |= F_CLOSEOPEN | F_FLATTEN | F_MASK;
    r[6] = 0, r[7] = SPEC_FILL;
    via_drawv(s, r);
    if (!s->v)
        s->r[0] = r[0];                         /* (the original pulls r0-r3 back) */
}

static void strokeclipped(struct ros_cpu *s)
{
    uint32_t cs = ros_ld32(s->r[7] + 4);
    os_error *e = style(&cs, F_FULLINTERIOR | F_INTERIORBDRY);
    if (e) {
        fail(s, e);
        return;
    }
    clip_path(s, s->r[7], s->r[3]);
    uint32_t r[8];
    memcpy(r, s->r, sizeof r);
    if ((e = stroke_regs(r, s->r[8] & 1, F_MASK))) {
        fail(s, e);
        return;
    }
    via_drawv(s, r);
}

/* ---- DrawV --------------------------------------------------------------------------------- */

static int draw_v(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    s->v = 0;
    switch (s->r[8]) {
    case 0: {
        uint32_t r0 = s->r[0];
        os_error *e = draw_process_path(&draw_ws, s->r, &r0);
        if (e)
            fail(s, e);
        else
            s->r[0] = r0;
        break;
    }
    case 1: fail(s, draw_err(DE_UNIMPLEMENTED)); break;
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
        reason(s);
        break;
    case 12: case 13: fillclipped(s); break;
    case 14: case 15: strokeclipped(s); break;
    default: fail(s, draw_err(DE_REASON)); break;
    }
    return ROS_VECTOR_CLAIM;
}

/* Every SWI goes through DrawV. R1-R11 are kept (mySWIhandler). */
static void via_vector(struct ros_cpu *s, uint32_t n)
{
    uint32_t keep[11];
    memcpy(keep, s->r + 1, sizeof keep);
    s->r[8] = n, s->r[9] = DRAWV, s->v = 0;
    ros_vector_call(DRAWV, s);
    memcpy(s->r + 1, keep, sizeof keep);
}

#define T(n, name) \
    void ros_thunk_Draw_##name(struct ros_cpu *s) { via_vector(s, n); }
T(0, ProcessPath) T(1, ProcessPathFP) T(2, Fill) T(3, FillFP) T(4, Stroke) T(5, StrokeFP)
T(6, StrokePath) T(7, StrokePathFP) T(8, FlattenPath) T(9, FlattenPathFP) T(10, TransformPath)
T(11, TransformPathFP) T(12, FillClipped) T(13, FillClippedFP) T(14, StrokeClipped)
T(15, StrokeClippedFP)
#undef T

/* ---- the module ---------------------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    return ros_vector_claim_native(DRAWV, draw_v, 0);
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    ros_vector_release_native(DRAWV, draw_v, 0);
    draw_freeworkspace(&draw_ws);
    free(draw_ws.clip);
    draw_ws.clip = NULL;
    draw_ws.clipsize = draw_ws.clipused = draw_ws.clipcount = 0;
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x110, "SWI value out of range for module Draw");
}

struct ros_module draw_module = {
    .title = "Draw",
    .help = "Drawing Module\t1.22 (08 May 2016) ROSGD native",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x40700,
    .swi_thunks = ros_swi_thunks_Draw,
    .swi_names = ros_swi_names_Draw,
    .swi_prefix = "Draw",
};

__attribute__((constructor)) static void count(void)
{
    draw_module.swi_count = ros_swi_count_Draw;
}
