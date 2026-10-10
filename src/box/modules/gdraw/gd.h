/* gd.h: what GDraw's files share (modules/gdraw).
 *
 * GDraw is Draw with a different last stage. Its path SWIs run native
 * Draw's ProcessPath (modules/draw) on a workspace of GDraw's own, with a
 * fill hook. The processed path's edges come to the hook instead of to
 * Draw's scan. The hook either hands them back to Draw's scan, which plots
 * exactly, or rasterises them itself with 4 x 4 point samples for
 * anti-aliasing. It rasterises into a coverage map, which is then blended
 * into the destination or kept as a clip region.
 */
#ifndef ROSGD_GD_H
#define ROSGD_GD_H

#include <stdint.h>

#include "rosgd/error.h"
#include "draw.h"

/* Style bits to which GDraw gives its own meaning */
#define GD_AA     0x80u                 /* anti-alias */
#define GD_BIT6   0x40u                 /* Draw's clipping "buffer" bit: ignored */

/* A coverage map. It has one byte per pixel, 0-16, and its rows run from the
 * bottom. */
struct gd_cov {
    uint8_t *c;
    int32_t x0, y0, w, h;               /* the pixels it covers: [x0, x0 + w) x [y0, y0 + h) */
};

/* What one operation is doing, and the module's state. */
struct gdraw {
    struct draw d;                      /* GDraw's own workspace for Draw's chain of stages */
    int claimed;                        /* DrawV claimed (reasons 61-63) */
    uint32_t clip;                      /* the current clip region: 0 for none, -1 as recorded (it does not clip), or a region */
    uint32_t fillstyle;                 /* the fill-style block (in the RMA), for ReadFillStyle */
    uint32_t printflag;                 /* the R0 given to SetPrintFlag, which is recorded */
    uint32_t ownbuf;                    /* GDraw's own region buffer (16 KB in the RMA), once claimed */
    uint32_t work;                      /* scratch space in the RMA for VDU variables and a palette */
    /* A graduated fill (SetFillStyle types 1 and 2 with bit 16). It is set,
     * the next Stroke gives its axis, and the plot after that uses it. */
    int grad;                           /* 0 none, GD_GRAD_ARMED, GD_GRAD_READY */
    uint32_t grad_type;                 /* 1 radial, 2 linear */
    uint32_t grad_table;                /* its colours, as COLORREFs (&TTBBGGRR) */
    int grad_dither;                    /* set when R0 bits 8-9 = 1: 2048 entries, 8 per step */
    double grad_sx, grad_sy, grad_ex, grad_ey;  /* the axis, in pixels */
    /* the operation in progress */
    int aa;                             /* set when the fill is anti-aliased */
    uint32_t region, regionsize;        /* when building a region: where it is and how much room there is */
    struct gd_cov *mark;                /* the coverage map that spanhook fills */
};
extern struct gdraw gdraw_ws;

#define GD_OWNBUF_SIZE 16384u
#define GD_GRAD_ARMED 1
#define GD_GRAD_READY 2

/* gdraw.c: the colour of a graduated fill at pixel (x, y), as a COLORREF */
uint32_t gd_grad_colour(int32_t x, int32_t y);

/* aa.c: the fill hook, regions and plotting */
os_error *gd_fillhook(struct draw *d);
os_error *gd_fill_region(uint32_t region, int32_t dx, int32_t dy);
int gd_region_valid(uint32_t region);

#endif
