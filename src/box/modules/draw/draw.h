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
 * (Sources/Video/Render/DrawMod: s.Draw, s.DrProcess, s.DrOutput, s.DrQFill).
 */

/* draw.h: what the Draw module's files share (modules/draw).
 *
 * Draw_ProcessPath passes each element of a path down a chain of stages
 * (s/Draw's "list of routines"). The stages are flatten, dash, thicken,
 * reflatten and transform, and then an output. The output is a fill, a
 * path, a bounding box or a count. A stage takes an element and the current
 * point. It returns the new current point in x0, y0, as the original's
 * routines return it in R0 and R1. The element's other fields may come back
 * changed.
 */
#ifndef ROSGD_DRAW_H
#define ROSGD_DRAW_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

extern struct ros_module draw_module;

/* ---- errors (the Draw module's, &980-&98B, &9FF) ------------------------------------- */

enum {
    DE_IRQ = 0x980, DE_REASON, DE_RESERVED, DE_ADDRESS, DE_ELEMENT, DE_SEQUENCE,
    DE_MAYEXPAND, DE_PATHFULL, DE_NOTFLAT, DE_CAPSJOINS, DE_OVERFLOW, DE_GRAPHICS,
    DE_UNIMPLEMENTED = 0x9FF,
};
os_error *draw_err(uint32_t n);

/* ---- path elements and flags (hdr/Draw) ----------------------------------------------- */

enum {
    ET_END, ET_START, ET_MOVE, ET_SPECIALMOVE, ET_CLOSEGAP, ET_CLOSELINE, ET_BEZIER,
    ET_GAP, ET_LINE,
};
#define ET_CONTINUE ET_START

enum {
    F_RULE = 3, F_FULLEXTERIOR = 4, F_EXTERIORBDRY = 8, F_INTERIORBDRY = 0x10,
    F_FULLINTERIOR = 0x20, F_BUFFER = 0x40, F_MASK = 0x80, F_INSIDE = 0x100,
    F_STYLEMASK = 0xFF,
};
#define F_FLAGSMASK     0xFE000000u
#define F_R7ISBBOX      0x02000000u
#define F_R7IS32BIT     0x04000000u
#define F_CLOSEOPEN     0x08000000u
#define F_FLATTEN       0x10000000u
#define F_THICKEN       0x20000000u
#define F_REFLATTEN     0x40000000u
#define F_FLOAT         0x80000000u

enum { SPEC_INSITU, SPEC_FILL, SPEC_FILLBYSUBPATHS, SPEC_COUNT };

/* ---- the chain -------------------------------------------------------------------------- */

struct draw_el {
    uint32_t type;
    int32_t x0, y0;                     /* the current point (R0, R1) */
    int32_t c1x, c1y, c2x, c2y;         /* a Bezier's control points (R2-R5) */
    int32_t x, y;                       /* the element's point (R6, R7) */
};

struct draw;
typedef os_error *draw_stage(struct draw *d, int i, struct draw_el *e);

/* The Draw module's workspace, as far as it matters here. */
struct draw {
    /* ProcessPath's arguments */
    uint32_t inputptr, flags, matrix;
    int32_t flatness, thickness;
    uint32_t joinsandcaps, dashptr, outputtype;
    draw_stage *chain[8];
    int nchain;
    int32_t subpathstart[2];
    uint32_t subpathtype;
    int32_t flatlimit;
    int32_t before[2], after[2];        /* the cache of the last transformed point */
    uint32_t joincapstate;
    int32_t initialvertex[2], initialoffset[2], finaloffset[2], currentoffset[2];
    int32_t circlecontrol;
    uint32_t dashstate, dashindex, dashdistance;
    int32_t userspacewinding;
    uint32_t fillstyle;
    int32_t currentwinding;
    int32_t box[4];                     /* boundingbox */
    uint32_t changedboxaddr;
    int gotasubpath;
    /* the VDU variables */
    int32_t lcol, brow, rcol, trow, orgx, orgy, xeig, yeig;
    uint32_t modeflags;
    /* an output's pointer and count (R9, R10) */
    uint32_t outptr, outcnt;
    /* the fill's edges (output.c) */
    struct draw_edges *edges;
    /* ClippedFills: the buffered spans of the clipping path */
    int32_t *clip;
    uint32_t clipsize, clipused, clipcount;     /* bytes, bytes, spans */
    /* GDraw's hooks (modules/gdraw). They are NULL for Draw's own
     * workspace. A fill hands its edges to fillhook instead of scanning
     * them. The scan hands its spans to spanhook instead of HLine. */
    os_error *(*fillhook)(struct draw *d);
    void (*spanhook)(struct draw *d, int32_t x0, int32_t y, int32_t x1);
};
extern struct draw draw_ws;

static inline os_error *draw_next(struct draw *d, int i, struct draw_el *e)
{
    return d->chain[i + 1](d, i + 1, e);
}

/* draw.c: ProcessPath itself, run on a workspace. Also the registers that
 * the other reasons give it (2-11: Fill, Stroke, StrokePath, FlattenPath,
 * TransformPath and their FP forms). R0-R7 are changed in place. */
os_error *draw_process_path(struct draw *d, const uint32_t r[8], uint32_t *r0);
os_error *draw_reason_regs(uint32_t reason, uint32_t r[8]);

/* process.c */
draw_stage process_float, process_transform, process_flatten, process_longedgeprotect,
    process_thicken, process_zerothicken, process_dash;

/* output.c */
draw_stage out_replace, out_fill, out_subpaths, out_count, out_box, out_path;
void draw_initbox(struct draw *d);
void draw_updatebox(struct draw *d, int32_t x, int32_t y);
void draw_updatechangedbox(struct draw *d);
void draw_freeworkspace(struct draw *d);
void draw_clip_sort(struct draw *d);
/* For a fillhook: the edges (the first is a dummy). Each edge has lower and
 * upper ends in 1/256 pixels, and a winding. Also Draw's own scan of the
 * edges. */
uint32_t draw_edge_count(const struct draw *d);
int32_t draw_edge(const struct draw *d, uint32_t k, int32_t w[4]);
void draw_scan(struct draw *d);

/* arith.c: DrArith's routines, and the kernel's heapsort (the one OS_HeapSort
 * uses) */
uint32_t draw_dsdivs(uint32_t lo, uint32_t hi, uint32_t d);
void draw_dsdivd(uint32_t lo, uint32_t hi, uint32_t d, uint32_t *qlo, uint32_t *qhi);
int32_t draw_dddivs(uint32_t lo, uint32_t hi, uint32_t dlo, uint32_t dhi);
void draw_dsmultd(uint32_t lo, uint32_t hi, int32_t s, uint32_t *rlo, uint32_t *rhi);
uint32_t draw_dsqrts(uint32_t lo, uint32_t hi);
void draw_ssmultd(int32_t a, int32_t b, uint32_t *lo, uint32_t *hi);
void draw_heapsort(uint32_t n, uint32_t *a, int (*less)(uint32_t x, uint32_t y, void *ctx),
                   void *ctx);

#endif
