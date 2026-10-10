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
 * This file is a reimplementation in C of the workspace layout of RISC OS
 * Open's Kernel source (Sources/Kernel: hdr.KernelWS, hdr.VduExt,
 * s.vdu.vduwrch).
 */
/* vduws.h -- the VDU drivers' workspace, shared by runtime/vdu's files.
 *
 * This is the kernel's VduDriverWorkSpace (Kernel/hdr/KernelWS), cut down
 * to what the VDU drivers here keep. The kernel's names are in the
 * comments so that each value can be found in its sources. Values that the
 * kernel keeps as byte offsets into the screen are kept here as columns and
 * rows.
 */
#ifndef ROSGD_VDUWS_H
#define ROSGD_VDUWS_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

/* Mode variables, with OS_ReadModeVariable's numbers (Kernel/hdr/VduExt). */
enum {
    MV_FLAGS, MV_SCRRCOL, MV_SCRBROW, MV_NCOLOUR, MV_XEIG, MV_YEIG, MV_LINELENGTH,
    MV_SCREENSIZE, MV_YSHFT, MV_LOG2BPP, MV_LOG2BPC, MV_XWIND, MV_YWIND, MV_COUNT
};

/* ModeFlags */
#define MF_NONGRAPHIC      (1u << 0)
#define MF_TELETEXT        (1u << 1)
#define MF_GAP             (1u << 2)
#define MF_BBCGAP          (1u << 3)
#define MF_HIRESMONO       (1u << 4)
#define MF_DOUBLEVERTICAL  (1u << 5)
#define MF_HARDSCROLLOFF   (1u << 6)
#define MF_FULLPALETTE     (1u << 7)
#define MF_INTERLACED      (1u << 8)
#define MF_GREYSCALE       (1u << 9)
#define MF_FORMAT          (0xFu << 12)
#define MF_RGB             (1u << 14)
#define MF_ALPHA           (1u << 15)

/* CursorFlags (Kernel/s/vdu/vduwrch) */
#define CF_81COLUMN  (1u << 0)      /* pending newline at the right edge */
#define CF_DIRECTION (0xFu << 1)    /* bits 1-4: reversed, swapped, wrap */
#define CF_NOMOVE    (1u << 5)      /* don't move the cursor after a character */
#define CF_INWRCH    (1u << 8)      /* the VDU is drawing: VSync keeps off */
#define CF_SPLIT     (1u << 18)     /* cursor editing */
#define CF_TELETEXT  (1u << 19)
#define CF_PAGEMODE  (1u << 20)
#define CF_CLIPBOX   (1u << 21)     /* ChangedBox is on (ClipBoxEnableBit) */
#define CF_ACTUAL    (1u << 25)     /* the cursor is on the screen */
#define CF_DISABLED  (1u << 26)     /* VDU 21 */
#define CF_TEUPDATE  (1u << 29)
#define CF_VDU5      (1u << 30)
#define CF_C81       (1u << 31)     /* the pending newline itself */

/* VduStatus */
#define VS_VDU2      (1u << 0)
#define VS_WINDOWING (1u << 3)
#define VS_SHADOW    (1u << 4)

struct vdu {
    /* The mode: its variables, and what they make. */
    uint32_t mv[MV_COUNT];
    uint32_t mode_no;           /* ModeNo: a number, or the selector's copy */
    uint32_t selector;          /* the copy, in an RMA block */
    int screen_ok;              /* a mode is set on a GraphicsV driver */
    uint8_t *screen;            /* ScreenStart */
    uint32_t screen_start, total_size;
    uint32_t screen_base;       /* the framestore's start: bank 1 */
    uint32_t display_start;     /* DisplayStart: the bank OS_Byte 113 shows */
    uint32_t bpp, bpc;          /* BitsPerPix; BytesPerChar, bits per character pixel */
    uint32_t char_width;        /* CharWidth, in bytes per text column */
    uint32_t tchar_x, tchar_y;  /* TCharSizeX (16 in teletext), TCharSizeY */
    uint32_t row_mult;          /* RowMult, in pixel rows per text row */
    uint32_t row_length;        /* RowLength, in bytes per text row */
    uint32_t text_offset;       /* TextOffset: teletext's page centred on the screen */
    uint32_t cursor_fill;       /* CursorFill */

    /* text */
    int32_t twl, twb, twr, twt; /* TWLCol, TWBRow, TWRCol, TWTRow */
    int32_t cx, cy;             /* CursorX, CursorY */
    int32_t icx, icy;           /* InputCursorX and InputCursorY. This is the
                                   second cursor, which cursor editing moves
                                   about the screen while CF_SPLIT is set */
    uint32_t cursor_flags;      /* CursorFlags */
    uint32_t status;            /* VduStatus */
    uint32_t tfore, tback;      /* TForeCol, TBackCol */
    uint32_t tftint, tbtint, gftint, gbtint;
    uint32_t text_fg, text_bg;  /* TextFgColour and TextBgColour. Each is a colour
                                   number, which SetColours then spreads through
                                   the word */
    uint32_t page_lines;        /* PageModeLineCount */

    /* Graphics: windows in internal (pixel) co-ordinates. */
    int32_t gwl, gwb, gwr, gwt; /* GWLCol, GWBRow, GWRCol, GWTRow */
    int32_t orgx, orgy;         /* OrgX, OrgY */
    int32_t gcsx, gcsy;         /* GCsX, GCsY: external */
    int32_t gcsix, gcsiy;       /* GCsIX, GCsIY: internal */
    int32_t olderx, oldery, oldx, oldy, newptx, newpty;
    uint32_t gplfmd, gplbmd, gfcol, gbcol;
    uint32_t gchar_sx, gchar_sy, gchar_spx, gchar_spy;
    uint32_t aspect;            /* AspectRatio: 0 square, 1 wide pixels, 2 tall */

    /* Colours and patterns for plotting (SetColour). */
    uint32_t fg_ecf[8], bg_ecf[8];          /* FgEcf and BgEcf, the patterns unrotated */
    uint32_t *fg_oe, *bg_oe, *bg_store;     /* FgEcfOraEor, BgEcfOraEor and BgEcfStore.
                                               Each is 8 {or, eor} rows in the RMA */
    uint32_t oe_tables;                     /* their address (GcolOraEorAddr) */
    uint32_t fg_pattern[8], bg_pattern[8];  /* the patterns that OS_SetColour sets */
    uint8_t ecf[4][8];                      /* Ecf1-4 */
    uint32_t bbc_ecfs;                      /* BBCcompatibleECFs: 0 BBC, else native */
    uint32_t ecf_shift, ecf_yoffset;        /* ECFShift, ECFYOffset: the ECF origin */

    /* The dot-dash line style. */
    uint64_t dot_style;                     /* DotLineStyle, with the first pixel in bit 63 */
    uint32_t dot_length, dot_cnt;           /* DotLineLength, LineDotCnt */
    uint64_t dot_pat;                       /* LineDotPat */

    /* The queue. */
    uint32_t qq;                /* QQ, the parameters. They are in the RMA for UKVDU23V */
    uint8_t qcode;              /* the code whose parameters are coming */
    int qwant, qlen;            /* VDUqueueItems is -qwant */

    /* The soft font: characters 32-255. */
    uint8_t font[224][8];

    /* The cursor. */
    uint32_t cur_start, cur_end;    /* the cell's pixel rows, [start, end) */
    uint32_t reg10copy, cur_speed, cur_counter, cur_desired;
    uint32_t cur_stack;             /* CursorStack */

    /* The display's mode (Display*), kept when output goes to a sprite. */
    uint32_t dmv[MV_COUNT];
    uint32_t dmode_no, dscreen_start;

    /* Sprites: the system area, SpChoose, and where output goes. */
    uint32_t sp_area;                   /* SpAreaStart, which is 0 or the system sprite area */
    uint32_t sp_choose;                 /* SpChoosePtr */
    uint8_t sp_choose_name[13];         /* SpChooseName */
    uint32_t dest_select;               /* SpriteMaskSelect: &23C, &23D */
    uint32_t dest_area, dest_sprite;    /* VduSpriteArea and VduSprite. 0 means the screen */
    uint32_t save_area;                 /* VduSaveAreaPtr, which is 0, 1 for the MOS's, or an address */

    /* The palette is &BBGGRRSS, in two flash states. Entry 256 is the
     * border and 257-259 are the pointer's. */
    uint32_t pal[2][260];
    int flash_state;                    /* the state shown: 0 is the first and 1 is the second.
                                           This is the kernel's FlashState inverted.
                                           FlashCount and the periods are OS_Byte &C1-&C3 */
    uint32_t blanked;                   /* ScreenBlankFlag: 1 blanked (PaletteV 6) */

    /* The virtual display that this context is (vdisplay.c), or NULL for
     * the real display's. It answers "is the VDU virtual now". */
    struct vdisplay *vd;
};

extern struct vdu vdu;

extern const uint8_t ros_vdu_hard_font[224][8];

/* modes.c */
os_error *vdu_mode_vars(uint32_t mode, uint32_t mv[MV_COUNT]);
os_error *vdu_set_mode(uint32_t mode);
void vdu_derive(void);              /* sets the workspace from the mode variables */
void vdu_default_windows(void);     /* VDU 26 */

/* text.c */
void vdu_set_colours(void);         /* SetColours, making the words from the pixel values */
void vdu_default_colours(void);     /* DefaultColours */
void vdu_compile_fg(void), vdu_compile_bg(void);
uint32_t vdu_gcol_to_colour(uint32_t gcol);     /* ConvertGCOLToColourNumber */
void vdu_paint_char(const uint8_t glyph[8]);
void vdu_clear_box(int32_t l, int32_t t, int32_t r, int32_t b);
void vdu_cls(void);
void vdu_scroll_up(void), vdu_scroll_down(void);
void vdu_scroll(uint32_t m, uint32_t d, uint32_t z);    /* VDU 23,7 */
void vdu_cursor_init(void);         /* InitCursor */
void vdu_prog_reg10(uint32_t v, int copy);
void vdu_cursor_end(uint32_t v);
void vdu_cursor_eor(void);
void vdu_pre_wrch(void), vdu_post_wrch(void);
void vdu_cursor_unsplit(void);      /* Vdu15: a carriage return ends editing */
uint32_t vdu_read_char(void);       /* OS_Byte 135: the character at the cursor */

/* ttx.c: teletext, while CursorFlags has CF_TELETEXT */
void vdu_ttx_init(int alloc);       /* TeletextInit, for a new mode's map (1) or only its settings */
void vdu_ttx_final(void);           /* TeletextFinalise */
void vdu_ttx_wrch(uint32_t c);      /* TTXDoChar: at the cursor, which stays */
void vdu_ttx_cls(void);             /* TTXFastCLS, and the colours to clear with */
void vdu_ttx_clear_box(int32_t l, int32_t t, int32_t r, int32_t b);    /* TTXClearBox */
void vdu_ttx_scroll(int up);        /* TTXSoftScrollUp and Down, for the text window */
void vdu_ttx_scroll_side(int left, int screen);     /* TTXScrollLeft and Right */
int vdu_ttx_vdu23_18(uint32_t n, uint32_t p);       /* 0 if n is not 0-3 */
uint32_t vdu_ttx_read_char(void);   /* TTXReadCharacter */
void vdu_ttx_vsync(void);           /* TeletextFlashTest */
/* Teletext's state, for a context to keep (vdisplay.c). It is at most
 * VDU_TTX_STATE bytes. A kept copy's map is freed. */
#define VDU_TTX_STATE 64
void vdu_ttx_state_save(void *to);
void vdu_ttx_state_load(const void *from);
void vdu_ttx_state_free(void *kept);

/* plot.c */
void vdu_plot_init(void);           /* allocates the tables' memory */
void vdu_plot_mode(void);           /* a mode's defaults: ECFs, line style and ECF origin */
void vdu_set_colour(void);          /* SetColour, making the patterns and OR/EOR tables */
os_error *vdu_plot(uint32_t k, int32_t x, int32_t y);   /* VDU 25, returning UKPLOTV's error */
void vdu_clg(void);                 /* VDU 16 */
void vdu_ecf_complex(uint32_t n, const uint8_t b[8]);   /* VDU 23,2-5 */
void vdu_ecf_simple(uint32_t n, const uint8_t b[8]);    /* VDU 23,12-15 */
void vdu_ecf_default(void);         /* VDU 23,11 */
void vdu_ecf_origin(int32_t x, int32_t y);          /* VDU 23,17,6 */
void vdu_line_style(const uint8_t b[8]);            /* VDU 23,6 */
void vdu_dot_length(struct ros_cpu *s);             /* OS_Byte 163,242 */
void vdu_ieg(void);                 /* GCsX/Y from GCsIX/Y */

/* The changed box (OS_ChangedBox) is ClipBoxEnable and ClipBoxLCol to
 * TRow. They are five words in the RMA whose address OS_ChangedBox hands
 * out. The drivers add what they draw only while CF_CLIPBOX is set. */
uint32_t *vdu_cbox(void);
void vdu_cbox_merge(int32_t l, int32_t b, int32_t r, int32_t t);    /* MergeClipBox */
void vdu_cbox_points(const int32_t *xy, int n);  /* MergeR11PointsFromR10, for the box of n points
                                                    within the graphics window */
void vdu_cbox_full(void);                        /* SetClipBoxToFullScreen */
void vdu_cbox_text(int32_t l, int32_t b, int32_t r, int32_t t);     /* ClipTextArea */
void vdu_cbox_circle(uint32_t rad, int32_t cx, int32_t cy);         /* ClipCircle */

/* Whether the drivers merge what they draw into a box. They do so for the
 * program's changed box while it is on. They also do so for a virtual
 * display's own dirty box whenever output goes to its screen
 * (vdisplay.c). */
#define VDU_CLIPPING ((vdu.cursor_flags & CF_CLIPBOX) || (vdu.vd && !vdu.dest_sprite))

/* sprite.c, sprplot.c, sprout.c */
void vdu_sprite_init(void);
void vdu_sprite_moved(uint32_t from, uint32_t to, uint32_t bytes, int32_t by);
os_error *vdu_output_to_screen(void);   /* called before a mode change */
uint8_t *vdu_mos_area(void);            /* save area 1, 384 bytes, one for each context */
void vdu_plot_sprite(uint32_t k);       /* PLOT &E8-&EF */
void vdu_vdu23_27(uint32_t op, uint32_t n);

/* vdu5.c */
void vdu5_char(uint32_t c);         /* draws a character at the graphics cursor */
void vdu5_delete(void);
void vdu5_control(uint8_t c);       /* 8-13, 30 */
void vdu5_tab(uint32_t x, uint32_t y);

/* palette.c */
void vdu_palette_init(void);        /* PaletteV's owner */
void vdu_palette_default(void);     /* PalInit */
void vdu_palette_unblank(void);     /* UnblankScreen */
void vdu_palette_vdu19(const uint8_t q[5]);
void vdu_palette_flash(void);
void vdu_palette_period(int second, struct ros_cpu *s);     /* OS_Byte 9, 10 */
void vdu_palette_read(struct ros_cpu *s);       /* OS_ReadPalette */
void vdu_palette_word11(uint32_t block), vdu_palette_word12(uint32_t block);
void vdu_palette_bulk(const uint32_t *entries, uint32_t n, uint32_t type);  /* PaletteV 8 */
void vdu_palette_set(uint32_t l, uint32_t type, uint32_t colour);         /* PaletteV 2 */

/* pointer.c */
void vdu_pointer_init(void);
void vdu_pointer_mode(void);        /* SetMouseRectangle, the pointer off */
void vdu_pointer_vsync(void);       /* PollPointer, UpdatePointer */
void vdu_pointer_word(uint32_t block);          /* OS_Word 21 */
void vdu_pointer_select(struct ros_cpu *s);     /* OS_Byte 106 */

/* GraphicsV. It returns 1 if a driver claimed the call. The registers
 * r[0] to r[3] are in and out. For a virtual display it is never called,
 * and it returns 0 as if no driver claimed it. */
int vdu_graphicsv(uint32_t reason, uint32_t r[4]);

/* text.c: a VSync's flashing for the live context, without the pointer.
 * It covers the cursor, the palette and teletext's banks. */
void vdu_flash_vsync(void);

/* vdisplay.c: virtual displays, for the drivers. Each acts on vdu.vd, the
 * live context's display. */
struct vdisplay;
/* A mode change's screen. The area is made big enough for two banks of the
 * mode, and the banks' sprites are written. It returns the first bank's
 * image and both banks' size. */
os_error *vdisplay_framestore(const uint32_t mv[MV_COUNT], uint32_t w, uint32_t h,
                              uint32_t *start, uint32_t *total);
void vdisplay_mode_changed(void);           /* the context was reset, so the whole display changed */
void vdisplay_dirty(int32_t l, int32_t b, int32_t r, int32_t t);    /* pixels, inclusive */
void vdisplay_palette_changed(void);
uint32_t vdisplay_display_start(void);      /* DisplayStart: the bank shown */
int vdisplay_byte(struct ros_cpu *s);       /* OS_Byte 112, 113: 1 if done */
void vdisplay_mouse(int32_t *x, int32_t *y, uint32_t *buttons);     /* in OS units, mapped */
void vdisplay_vsync(void);                  /* each virtual display's flashing */

#endif
