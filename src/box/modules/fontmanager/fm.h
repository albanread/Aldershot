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
 * (Sources/Video/Render/Fonts/FontManager: s.Errors, s.Fonts, s.ListFonts, hdr.Font).
 */

/* fm.h: what the Font Manager's files share. */
#ifndef ROSGD_FM_H
#define ROSGD_FM_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

extern struct ros_module fontmanager_module, romfonts_module;

/* ---- errors (s/Errors, from &200; texts from Resources/UK/Messages) ------ */

enum {
    FE_NOROOM = 0x200, FE_CACHEFULL, FE_NOCACHE, FE_TOOLONG, FE_64K, FE_PALTOOBIG, FE_BADTRANBITS,
    FE_NOTENOUGHBITS, FE_NOFONT, FE_NOPIXELS, FE_BADFONTNUMBER, FE_NOTFOUND, FE_BADFONTFILE,
    FE_NOHANDLES, FE_BADCOUNTER, FE_BADCTRLCHAR, FE_SINUSE, FE_BADSEGMENT, FE_BADPREFIX,
    FE_RESERVED, FE_BADCHARCODE, FE_NOBITMAPS, FE_NOBITMAPS2, FE_BADCACHEFILE, FE_FIELDNOTFOUND,
    FE_BADMATRIX, FE_OVERFLOW, FE_DIVBY0, FE_BADREADMETRICS, FE_BADRGB, FE_ENCNOTFOUND,
    FE_MUSTHAVESLASH, FE_BADENCSIZE, FE_TOOMANYIDS, FE_TOOFEWIDS, FE_NOBASEENC, FE_IDNOTFOUND,
    FE_TOOMANYCHUNKS, FE_BADFONTFILE2, FE_SUPREMACY, FE_NOPATHDOT,
};
/* two more errors that share FE_NOTFOUND's number but have texts of
 * their own */
enum { FE_DATANOTFOUND = 0x10000 | FE_NOTFOUND, FE_DATANOTFOUND2 = 0x20000 | FE_NOTFOUND };
#define FE_CANTKILL 0x103u
#define FE_BUFFOVERFLOW 0x1E4u
#define FE_NOSUCHSWI 0x1E6u
#define FE_BADPARM 0x1EAu       /* the kernel's "Bad parameters" */

/* The number of zero bytes after a font file's data in memory (fm_load) */
#define FM_PAD 32

/* The error, with %0 and %1 substituted.  Either may be NULL. */
os_error *fm_err(uint32_t n, const char *p0, const char *p1);

/* ---- the catalogue: Font$Path's prefixes and what they hold -------------- */

enum { LFFF_DEFAULT = 1, LFFF_ENCODING = 2, LFFF_SYMFONT = 4, LFFF_LANGFONT = 8 };
enum { LFPR_SCANNED = 1, LFPR_ISBAD = 2 };

struct fm_prefix {
    struct fm_prefix *next;
    uint8_t flags;
    char prefix[256];
};

struct fm_block {                       /* a font or an encoding (lff_) */
    struct fm_block *next;
    uint32_t flags;
    uint32_t territory;                 /* 0 if the name is the identifier */
    struct fm_prefix *prefix;
    char *id, *name;                    /* name may be id */
};

extern struct fm_block *fm_catalogue;   /* sorted: by name, then by prefix */
extern uint32_t fm_territory;

/* findblockfromID_noqual: the first block of the kind whose identifier
 * matches id (terminated by '\', space or a control character) */
os_error *fm_find_block(const char *id, int encoding, struct fm_block **b);
/* findblockfromID: the \F (or \E) field of s, or s itself */
os_error *fm_find_block_field(const char *s, int encoding, struct fm_block **b);
os_error *fm_recache(void);             /* try_listfonts_recache */
/* comparefontid_gotfield: 0 if equal */
int fm_compare_id(const char *block_id, const char *cand);
/* findfield: the text after "\<c>", or NULL.  For c == 'F' the bare string
 * counts too. */
const char *fm_find_field(const char *s, int c);
/* get_name_from_id, for error texts */
void fm_name_from_id(const char *s, int encoding, char *out, size_t size);

void fm_thunk_ListFonts(struct ros_cpu *s);
void fm_thunk_DecodeMenu(struct ros_cpu *s);

/* ---- fonts ------------------------------------------------------------- */

enum { MSF_NORMAL = 0, MSF_RAMSCALED = 1, MSF_MASTER = 2 };
enum { BASE_NONE = -1, BASE_UNKNOWN = -2, BASE_DEFAULT = -3, BASE_PRIVATE = -4 };

/* Where a kind of pixel data comes from (hdr1_/hdr4_leafname's first byte) */
enum {
    LEAF_NONE, LEAF_SCAN,               /* no data; not yet decided */
    LEAF_4BPP,                          /* scaled from the master's 4-bpp bitmaps */
    LEAF_DIRECT,                        /* the master's outlines, drawn directly */
    LEAF_OUTLINES,                      /* the outlines, made into bitmaps in the cache; PP_4X/YPOSNS may be
                                           added */
    LEAF_FILE = 8,                      /* from a file: name[] */
};
enum {
    PP_4XPOSNS = 1, PP_4YPOSNS = 2,     /* for bitmaps: subpixel positions */
    PP_16BITSCAFF = 1, PP_MONOCHROME = 2, PP_FILLNONZERO = 4, PP_BIGTABLE = 8,  /* version 8 outlines */
    PP_FLAGSINFILE = 0x40, PP_DEPENDENCIES = 0x80,
};

struct fm_chunk;

struct fm_leaf {                        /* one kind of pixel data: hdr1_ (outlines and 1-bpp) or hdr4_ */
    uint8_t type;
    char name[12];                      /* LEAF_FILE: the leafname */
    uint8_t flags;                      /* PP_* */
    uint32_t pixoffstart, nchunks, nscaffolds;
    uint32_t address;                   /* the data's address if it is in ResourceFS, else 0 */
    int32_t box[4];                     /* x0, y0, x1, y1 */
    uint8_t *data;                      /* LEAF_FILE: the file, once its chunks are wanted */
    uint32_t len;
    struct fm_chunk **chunks;           /* LEAF_OUTLINES: the bitmaps made so far (pixels.c) */
    uint32_t nchunked;                  /* chunks[]'s size */
};

/* ---- arithmetic (arith.c) ---------------------------------------------- */

struct fm_fp { uint32_t m, w; };        /* a float in a matrix: the mantissa, then the exponent with the sign */
struct fm_fpmat { struct fm_fp v[6]; }; /* XX YX XY YY X Y */
struct fm_mat { int32_t v[6], cs; };    /* fixed point: XX to YY are 16.16 shifted right by cs, then X and Y; cs is
                                           the coordshift */

void fm_matrix_float(const int32_t in[6], struct fm_fpmat *out);           /* matrix_float */
/* matrix_multiply: a then b.  If either is NULL, the result is the other. */
os_error *fm_matrix_mul(const struct fm_fpmat *a, const struct fm_fpmat *b, struct fm_fpmat *out);
os_error *fm_matrix_fix(const struct fm_fpmat *in, struct fm_mat *out);    /* matrix_fix */
void fm_matrix_double(struct fm_fpmat *m);                                  /* matrix_double */
struct fm_fpmat fm_size_matrix(int32_t xsize, int32_t ysize);              /* getxysizeover16 */
os_error *fm_design_matrix(int32_t designsize, struct fm_fpmat *out);      /* getdesignmatrix */
os_error *fm_res_value(int32_t dpi, struct fm_fp *out);                    /* div72000 */
struct fm_fpmat fm_res_matrix(struct fm_fp xx, struct fm_fp yy);
void fm_transform_pt(const struct fm_mat *m, int32_t *x, int32_t *y);      /* transformpt */
void fm_transform_box(const struct fm_mat *m, int32_t box[4]);             /* transformbox */
void fm_transform_xyscale(const struct fm_mat *render, int32_t designsize, int32_t *xscale,
                          int32_t *yscale, uint8_t *swap);                   /* transformxyscale */

struct fm_trn {                         /* a transform block: the data for a paint matrix (trn_) */
    struct fm_trn *next;
    int32_t pm[6];                      /* trn_paintmatrix */
    int bpp1;                           /* in the 1-bpp chain, because it was made while monochrome */
    struct fm_leaf leaf;                /* trn_leafname, its box and chunks */
    int render_ok;                      /* trn_rendermatrix known */
    struct fm_mat met, bbox, render;
    int32_t xscale, yscale;
    uint8_t flags;                      /* trn_flags: bit 0 swaps the subpixel positions */
};
extern struct fm_trn *fm_trn;           /* transformptr: the block in use, or NULL */

struct fm_font {                        /* the font header (hdr_) */
    int usage;
    uint8_t flags;                      /* bit 0: swap the x/y subpixel positions */
    char name[40];                      /* CR-padded */
    int32_t xsize, ysize;               /* 1/16 point.  A master's are the sizes in its metrics. */
    uint32_t nchars;
    uint8_t metflags, skelthresh;
    uint32_t masterfont;                /* handle, or 0 for none */
    int masterflag;
    uint8_t encoding[12];
    int32_t base;
    int32_t xmag, ymag;
    int32_t xscale, yscale, xres, yres;
    int32_t threshold[5];
    uint32_t metoffset, metsize, pixoffset, pixsize, scaffoldsize;
    int32_t designsize;
    int have_matrix;
    int32_t matrix[6];                  /* \M: four values in 16.16, then two in millipoints */
    char *pathname, *pathname2;         /* "<prefix><name>." */
    struct fm_leaf leaf4, leaf1;
    char *charlist;                     /* a RAM-scaled font's */
    uint32_t metaddress;
    uint32_t age;                       /* when it was last claimed or lost */
    /* the matrices (metrics.c) */
    struct fm_fp resxx, resyy;          /* hdr_resXX/YY: (dpi << 9) / 72000 */
    int render_ok;                      /* hdr_rendermatrix is known (not MAT_MARKER) */
    struct fm_mat render, bbox;         /* hdr_rendermatrix, hdr_bboxmatrix */
    struct fm_mat metmat;               /* the metrics matrix from the \M matrix */
    /* a master's cached data (metrics.c) */
    uint8_t *metrics;                   /* the metrics file from offset 48 (met_nchars on) */
    uint32_t metlen;
    uint8_t *kerns;                     /* CacheKerns' block, from kern_index */
    uint32_t kernsize;                  /* its size from kern_index: 0 if not cached, 1 if none */
    uint32_t oldkernsize;               /* hdr_oldkernsize */
    struct fm_trn *trns;                /* hdr_transforms, the most recently used first */
};

extern struct fm_font *fm_fonts[256];   /* 1..255 */
extern uint32_t fm_currentfont, fm_futurefont;
extern int32_t fm_threshold[5];         /* FontMax1..5 */
extern uint32_t fm_maxcache;            /* FontMax */

os_error *fm_font_ptr(uint32_t h, struct fm_font **f);    /* getfontheaderptr */
void fm_thunk_FindFont(struct ros_cpu *s);
void fm_thunk_LoseFont(struct ros_cpu *s);
void fm_thunk_ReadDefn(struct ros_cpu *s);
void fm_thunk_LookupFont(struct ros_cpu *s);
void fm_thunk_ReadFontPrefix(struct ros_cpu *s);
void fm_set_thresholds(struct fm_font *f);                  /* setthresholds */
void fm_reset_fontmax(void);                                /* resetfontmax */
void fm_lose_font(uint32_t h);                              /* Font_LoseFont */
/* constructname: the master's directory followed by a leafname.  For a
 * leafname that starts with 'O', such as "Outlines", the directory is
 * pathname2 if there is one. */
os_error *fm_file_name(struct fm_font *f, const char *leaf, char *out, size_t size);
/* setleafnames: "IntMetrics" and "Outlines", with the base's number added
 * if there is one */
os_error *fm_leafnames(struct fm_font *f, char metrics[12], char outlines[12]);

/* ---- metrics (metrics.c) ------------------------------------------------- */

/* The paint matrix is 6 words.  They are the caller's, the string's, or the
 * buffer that a control 27 fills.  NULL means the unit matrix.
 * oldpaintmatrix remembers the matrix that SetMetricsPtrs last set up for.
 * It is FM_NOMATRIX if there is none. */
extern const uint8_t *fm_paintmatrix, *fm_oldpaintmatrix;
#define FM_NOMATRIX ((const uint8_t *)-1)
void fm_set_paint_matrix(const uint8_t *m);                /* setpaintmatrix */
extern int32_t fm_paintmatrixbuffer[6];

/* SetMetricsPtrs' results for the current font (metricsptr and the rest) */
struct fm_metrics {
    int valid;                          /* metricsptr != 0 */
    const uint8_t *map;                 /* the character map */
    uint32_t mapsize;                   /* metchmapsize: 0 = none, code = index */
    uint32_t nchars;                    /* the number of characters that the arrays hold */
    uint8_t flags;                      /* metflags */
    const uint8_t *bbox[4];             /* bboxx0, y0, x1, y1: s16 arrays, or NULL */
    const uint8_t *xoff, *yoff;         /* xoffset, yoffset: s16 arrays, or NULL */
    const uint8_t *misc, *kerns;        /* metmisc, metkerns, or NULL */
    int32_t xscale, yscale, xfactor, yfactor;   /* x/ymetscale, x/ymetfactor */
    const struct fm_mat *matrix;        /* metricsmatrix, or NULL */
};
extern struct fm_metrics fm_met;
enum { MET_NOBBOXES = 1, MET_NOXOFFSETS = 2, MET_NOYOFFSETS = 4, MET_MOREDATA = 8,
       MET_MAPSIZED = 0x20, MET_16BITKERNS = 0x40 };

os_error *fm_set_metrics(void);                             /* SetMetricsPtrs */
void fm_scale_width(int32_t *x, int32_t *y);                /* scalewidth */
int32_t fm_scale_x(int32_t v);                              /* scalexwidth */
int32_t fm_scale_y(int32_t v);                              /* scaleyheight */
/* getoutlines_metricsbbox: a glyph's bbox from its outline, millipoints */
os_error *fm_outline_metrics_bbox(int32_t g, int32_t box[4]);
/* the master's kern block (CacheKerns), NULL if none; getkernpair */
os_error *fm_kerns(struct fm_font *master, const uint8_t **blk);
int fm_kern_pair(const uint8_t *blk, uint32_t l, uint32_t r, int32_t *kx, int32_t *ky);
/* getnewrendermatrix, and GetTransform's, for the header (no paint
 * matrix) */
os_error *fm_render_matrix(struct fm_font *f);
/* GetTransform: sets fm_trn for the paint matrix (NULL for none).  The
 * block is found, or made if there is none. */
os_error *fm_get_transform(struct fm_font *f);
void fm_forget_transforms(struct fm_font *f);                /* uncachetransforms: frees the transform blocks and their data */
/* masteroutlinebbox, for the leaf l of a slave */
os_error *fm_outline_box(struct fm_font *f, struct fm_leaf *l);
void fm_forget_data(struct fm_font *f);                     /* on deletion */
void fm_forget_metrics(struct fm_font *f);                  /* its metrics file has changed */
extern uint8_t fm_charflags;                                /* charflags; chf_1bpp is painting's */
enum { CHF_12BIT = 1, CHF_1BPP = 2, CHF_OUTLINES = 8, CHF_COMPOSITE1 = 0x10, CHF_COMPOSITE2 = 0x20,
       CHF_16BITCODES = 0x40 };
/* A chunk of a font file (setoutlineptr and setpixelsptr).  index is NULL
 * if the file has none. */
struct fm_chunkp {
    const uint8_t *index;               /* pix_index: offsets from here to the characters */
    uint32_t flags;                     /* pix_flags */
    const uint8_t *end;                 /* the end of the file's data, which no offset may pass */
};
os_error *fm_load_chunk(struct fm_font *f, struct fm_leaf *l, uint32_t c, struct fm_chunkp *ck);
const uint8_t *fm_char_in(const struct fm_chunkp *ck, uint32_t code);   /* NULL if none */
/* readbbox: x, y, w, h after the flags byte; the data after them */
const uint8_t *fm_read_bbox(const uint8_t *p, uint8_t flags, int32_t b[4]);
/* getbbox: a character's pixel box, x, y, w, h.  An outline's box goes
 * through the render matrix, with the margin that chf_1bpp in charflags
 * says. */
os_error *fm_bbox_pixels(struct fm_font *m, const struct fm_mat *render, const uint8_t *p,
                         int32_t b[4]);
void fm_thunk_CharBBox(struct ros_cpu *s);
void fm_thunk_ReadInfo(struct ros_cpu *s);
void fm_thunk_ReadFontMetrics(struct ros_cpu *s);

/* ---- strings (scan.c) ------------------------------------------------------ */

void fm_thunk_StringWidth(struct ros_cpu *s);
void fm_thunk_FindCaret(struct ros_cpu *s);
void fm_thunk_FindCaretJ(struct ros_cpu *s);
void fm_thunk_StringBBox(struct ros_cpu *s);
void fm_thunk_ScanString(struct ros_cpu *s);
/* the future state that a scan leaves (futuredata), and the current
 * colours */
struct fm_colours { uint32_t rgb_b, rgb_f, rgb_a; uint8_t fcol, bcol, acol; };
extern struct fm_colours fm_current, fm_future;
/* the string's state, shared by scanning and painting */
extern int32_t fm_xspaceadd, fm_yspaceadd, fm_xletteradd, fm_yletteradd, fm_xglueadd, fm_yglueadd;
extern uint32_t fm_extcode;                 /* externalcharcode */
struct fm_future { struct fm_colours c; uint32_t font; };
extern struct fm_future fm_tfuture;         /* tfuturedata: held until a split point */
extern int fm_futurechanged;
int fm_width(void);                         /* the plot type's units: 0 for 8-bit, 1 for 16, 2 for 32 */
uint32_t fm_read_uint(uint32_t *p);         /* readuint1 */
int32_t fm_read_int(uint32_t *p);           /* readint1 */
int32_t fm_read_int3(uint32_t *p);          /* readint3 */
uint32_t fm_read_rgb(uint32_t *p);          /* readRGB */
os_error *fm_next_char(uint32_t *pp, uint32_t *out);        /* readnextchar */
/* getcharwidth: g's advance (in millipoints, transformed), with the glue
 * after it set */
os_error *fm_char_width(int32_t g, uint32_t next, int32_t *w, int32_t *h);
struct fm_pars { uint32_t p; int32_t x, y, n; };
/* scanstring: gives the parameters at the last split point before the scan
 * stopped */
os_error *fm_scan_string(uint32_t p, int32_t limx, int32_t limy, int32_t split, int32_t maxidx,
                         int32_t spx, int32_t spy, struct fm_pars *out);

/* ---- colours (colours.c) -------------------------------------------------- */

/* The anti-aliasing thresholds (fontmanager.c).  The list for k+2 colours,
 * counting the background, is at offset(k). */
extern uint8_t fm_thresholds[135];
unsigned fm_thresh_offset(unsigned k);
/* outputdata: the pixel values of the 16 levels, at the top of each word.
 * Also outputmask, rubdata and setoutputdataflag. */
extern uint32_t fm_outputdata[16], fm_outputmask, fm_rubdata;
extern uint8_t fm_outputvalid;
extern uint8_t fm_bpp, fm_ppw;          /* bitsperpixel, Pixelsperword */
extern int fm_inscanstring;             /* Font_SetColourTable leaves outputdata alone */
extern uint32_t fm_switch_buffer;       /* Font_SwitchOutputToBuffer's buffer, or 0 */
extern uint32_t fm_switch_flags;        /* its flags: SWF_* */
enum { SWF_JUSTCOUNT = 1, SWF_SCAFFOLD = 2, SWF_NOBITMAPS = 16 };
#define SWF_ENABLED 0x80000000u
extern uint32_t fm_switch_fore, fm_switch_back;
extern uint32_t fm_plottype;            /* the current Font_Paint's flags */
void fm_depth(int32_t log2bpp);         /* the depth part of setmodedata */
/* Blending (Font_Paint bit 11).  kind is the pixel format. mode is 0 for
 * normal, 1 for supremacy and 2 for alpha.  fg is the foreground as a
 * pixel and fgalpha is its alpha (0 to 256).  ctable and itable are
 * InverseTable's tables, used at 8 bpp. */
enum { BLEND_NONE, BLEND_8, BLEND_1555, BLEND_565, BLEND_4444, BLEND_32 };
struct fm_blend { int kind, mode; uint32_t fg, fgalpha, ctable, itable; };
extern struct fm_blend fm_blend;
void fm_check_blend(void);              /* checkblend */
os_error *fm_set_output(void);          /* trysetoutputdata */
void fm_thunk_SetFontColours(struct ros_cpu *s);
void fm_thunk_SetPalette(struct ros_cpu *s);
void fm_thunk_ReadColourTable(struct ros_cpu *s);
void fm_thunk_SetColourTable(struct ros_cpu *s);
void fm_thunk_CurrentRGB(struct ros_cpu *s);
void fm_thunk_FutureRGB(struct ros_cpu *s);

/* ---- pixels (pixels.c) ------------------------------------------------------ */

#define PIX_ALLCHARS 0x80000000u        /* a whole chunk instead of one character */
enum { PIX_UNCACHED = 1 };              /* an index entry that is not made yet */
#define PP_SPLITCHUNK (1u << 17)
#define PP_INCACHE (1u << 18)
#define PP_FLAGSPRESENT 0x80000000u

struct fm_chunk {                       /* bitmaps made from outlines */
    uint32_t flags;                     /* pix_flags: PP_4X/YPOSNS, PP_SPLITCHUNK, PP_INCACHE */
    uint8_t *mem;                       /* the index, then the characters, laid out as in a file */
    uint32_t len;
};

/* setpixelsptr for a whole chunk: chunk c of the leaf l of a slave.  It is
 * made from its master's outlines if it is not made yet. *out is NULL if
 * there is none. */
os_error *fm_pixel_chunk(struct fm_font *f, struct fm_leaf *l, uint32_t c, struct fm_chunk **out);
void fm_delete_chunks(struct fm_leaf *l);
/* The end of the data of the bitmap character that fm_pixel_char last
 * returned */
extern const uint8_t *fm_pixel_end;
/* SetPixelsPtr for a character g of the leaf l.  This gives its header (the
 * flags byte), made from the outlines if it is not made yet. For a leaf
 * that is drawn directly, it gives the master's outline character, and
 * charflags then have chf_outlines and chf_1bpp.  *out is NULL if there
 * is none. */
os_error *fm_pixel_char(struct fm_font *f, struct fm_leaf *l, int bpp1, uint32_t g,
                        const uint8_t **out);
/* drawchar for painting: draws the outline at p (past its header) through
 * the Draw matrix at matrix, with the pen at 0,0 */
void fm_draw_outline(struct fm_font *f, const uint8_t *p, uint32_t code, uint32_t matrix, os_error **e);
void fm_restore_output(void);                               /* restoreoutput */

/* The VDU's hooks (vduhooks.c): VDU 23,26, VDU 23,25 and PLOT &D0 to &D7 */
os_error *fm_vdu_hooks_claim(void);     /* claimvectors */
void fm_vdu_hooks_release(void);
os_error *fm_settransfer(int32_t v, const uint8_t *src);   /* settransfer */
os_error *fm_vdu_palette(uint32_t q);   /* VDU 23,25,&80+b,... */
os_error *fm_define_font(uint32_t h, const char *s, int32_t xs, int32_t ys, uint32_t xres,
                         uint32_t yres);                    /* DefineFont */
os_error *fm_paint_vdu(uint32_t str, uint32_t plottype);   /* paintchars for PLOT &D0 to &D7 */
void fm_thunk_MakeBitmap(struct ros_cpu *s);
void fm_thunk_Paint(struct ros_cpu *s);                    /* paint.c */
void fm_thunk_Caret(struct ros_cpu *s);
void fm_thunk_SwitchOutputToBuffer(struct ros_cpu *s);
void fm_thunk_UnCacheFile(struct ros_cpu *s);

/* ---- the files (files.c) -------------------------------------------------- */

os_error *fm_metrics_header(struct fm_font *f);            /* GetMetricsHeader */
/* GetPixels4Header and GetPixels1Header.  If there is a transform block in
 * use (fm_trn), they use its leaf. bpp1 says which header is wanted. */
os_error *fm_pixels_header(struct fm_font *f, struct fm_leaf *which);
os_error *fm_pixels_header_bpp(struct fm_font *f, struct fm_leaf *which, int bpp1);
/* OS_File 17 on name: gives the object type, and *len is its length */
os_error *fm_file_type(const char *name, uint32_t *type, uint32_t *len);
/* The whole file, zero-terminated, in malloc memory */
os_error *fm_load(const char *name, uint8_t **data, uint32_t *len);

/* ---- encodings ---------------------------------------------------------- */

/* getencodingid: puts the target encoding of a font string into enc[12] */
os_error *fm_encoding_id(const char *s, uint8_t enc[12]);
os_error *fm_base_encoding(struct fm_font *f);            /* GetFontBaseEncoding */
/* mapchar: the glyph for a code in f, or -1 if none.  fm_map_r14 is what
 * the original leaves in R14, which is the code's offset in its run, or the
 * high byte of a table run's entry.  getgluewidth tests it as if it were
 * the plot type (see scan.c). */
os_error *fm_map_char(struct fm_font *f, int32_t code, int32_t *out);
extern uint32_t fm_map_r14;
void fm_forget_maps(struct fm_font *master);                /* its private-base mappings */
void fm_thunk_ReadEncodingFilename(struct ros_cpu *s);
void fm_thunk_EnumerateCharacters(struct ros_cpu *s);

/* ---- helpers ------------------------------------------------------------ */

uint32_t fm_swi(uint32_t n, uint32_t r[10], os_error **e);
uint8_t fm_lower(uint8_t c);            /* uk_LowerCase: A to Z */
uint8_t fm_lower_latin1(uint8_t c);     /* LowerCase: also &C0 to &D6 and &D8 to &DE */
os_error *fm_mode_vars(void);           /* setmodedata */
struct fm_vdu {                         /* what setmodedata reads */
    uint32_t scrtop;                    /* ScreenStart */
    int32_t gx0, gy0, gx1, gy1;         /* the graphics window in pixels, inclusive */
    int32_t cursors[8];                 /* OlderCs, OldCs, GCsI, NewPt: x, y each */
    uint32_t ncolour, modeflags, linelen;
    int32_t ywindlimit;
};
extern struct fm_vdu fm_vdu;
void fm_dividex(int32_t v, int32_t *pix, uint8_t *sub);    /* dividex */
void fm_dividey(int32_t v, int32_t *pix, uint8_t *sub);    /* dividey */
extern int32_t fm_xco72, fm_yco72, fm_xcoord, fm_ycoord, fm_oldxcoord, fm_oldycoord;
extern uint8_t fm_antialiasx, fm_antialiasy;
void fm_calcxcoord(void);               /* calcxcoord */
extern int32_t fm_xeig, fm_yeig, fm_log2bpp;
extern uint32_t fm_printerflag;
extern int32_t fm_xscalefactor, fm_yscalefactor;
int32_t fm_divide(int32_t a, int32_t b);                   /* divide: rounds down */

#endif
