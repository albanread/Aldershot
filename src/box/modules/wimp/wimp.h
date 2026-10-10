/* wimp.h -- the Window Manager, native.
 *
 * This Wimp is written afresh in C.  It is not translated.  RISC OS 5.88's
 * ObjAsm source (Sources/Desktop/Wimp) settles questions of behaviour, but
 * this code does not take its shape, in which registers serve as variables
 * and the workspace is a set of offsets.  It stands in the ROM beside the
 * translated Wimp, and starts in its place unless the box is booted with
 * rosgd.wimp=translated (runtime/rom.c).
 *
 * All its state is in the RMA, reached through the module's private word.
 * That is module.h's rule.  It keeps the state one desktop's state,
 * whichever thread (or, later, process) reads it.  Each task is a runtime
 * task (task.h).  The baton, the task's slot, its handlers and its
 * DomainId move with ros_task_switch, so the Wimp itself pages nothing in
 * and swaps no handlers.
 *
 * The Wimp's own records are kept apart from the screen, as a later window
 * server will need.  Nothing here reads back what it drew. */
#ifndef ROSGD_WIMP_H
#define ROSGD_WIMP_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/vdu.h"

struct ros_task;

#define WIMP_VERSION   588u              /* WindowManager 5.88 */
#define WIMP_TASKS     128u              /* the task table's slots, as 5.30's */
#define TASK_WORD      0x4B534154u       /* "TASK" */
#define WIMP_MAGIC     0x706D6957u       /* "Wimp": the receiver for its own windows */

/* Errors: the number and the UK text */
#define E_BAD_OP          0x281u         /* Invalid Wimp operation in this context */
#define E_BAD_HANDLE      0x288u         /* Illegal window handle */
#define E_BAD_VERSION     0x295u         /* Bad version number passed to Wimp_Initialise */
#define E_BAD_MESSAGE     0x296u         /* Message block is too big / not a multiple of 4 */
#define E_BAD_REASON      0x297u         /* Bad reason code */
#define E_BAD_TASK        0x298u         /* Illegal task handle */
#define E_BAD_SYSINFO     0x29Eu         /* Bad parameter passed to Wimp in R0 */
#define E_BAD_PTR_R1      0x29Fu         /* Bad pointer passed to Wimp in R1 */
#define E_BAD_R3          0x2A2u         /* Illegal Wimp_Poll pointer in R3 */
#define E_TOO_BIG         0x284u         /* not enough memory to create this window or menu */
#define E_GET_RECT        0x286u         /* Get_Rectangle not called correctly */
#define E_OWNER_WINDOW    0x29Bu         /* Access to window denied */
#define E_BAD_PARENT      0x2AAu         /* Bad parent window */

os_error *wimp_error(uint32_t num);

/* ---- tasks ---------------------------------------------------------------- */

/* A message list's state */
enum { MSGS_ALL, MSGS_NONE, MSGS_SET };

/* An event chosen for a task: what its Wimp_Poll returns */
struct wimp_event {
    uint32_t reason;
    uint32_t r2;                         /* R2 on exit, if set_r2 */
    int set_r2;
    uint32_t size;                       /* bytes of data copied into the block */
    uint8_t data[256];
};

struct wimp_task {
    uint32_t handle;                     /* external: version << 21 | internal */
    uint32_t internal;                   /* the low 16 bits: the DomainId */
    uint32_t generation;                 /* bits 21-30, new at each initialise */
    int live;
    uint32_t version;                    /* normalised: 0, 200, 300, 310, 380 */
    struct ros_task *rt;                 /* the runtime's task: the baton */
    char name[64];
    /* what it last asked of Wimp_Poll */
    uint32_t mask;                       /* bits 0-20, bit 20 PollIdle */
    uint32_t block;                      /* its poll block */
    uint32_t idle_time;                  /* PollIdle's R2 */
    uint32_t pollword;                   /* 0 for none */
    int pollword_fast;
    /* the messages it takes */
    int msgs_state;
    uint32_t nmsgs;
    uint32_t msgs[64];
    /* an event another task's poll chose for it, waiting for it to wake */
    int pending;
    struct wimp_event ev;
    /* Wimp_StartTask */
    uint32_t start_result;               /* the parent's R0 when it is released */
    char command[1024];                  /* a child's command, for its thread */
    /* the memory in use last told the Task Manager (Message_SlotSize) */
    uint32_t used_told;
    /* null-event pacing (task.c, #138), a deliberate change from 5.30 */
    uint64_t null_out_ns;                /* when its last null went to it, or 0 after any other event */
    uint64_t null_due_ns;                /* a paced task's next null: not before this */
    uint32_t null_quick;                 /* nulls in a row it polled again after in under 0.5 ms */
    int null_paced;                      /* this poll's nulls are paced */
    int null_held;                       /* a null was held back in this poll */
    uint32_t nulls, nulls_paced;         /* nulls delivered, and how many of them were held back first */
};

/* ---- the message queue ----------------------------------------------------- */

#define RECV_TASK      0                 /* recv is a task handle */
#define RECV_BROADCAST 1                 /* every slot in turn: cursor is the next */
#define RECV_NOBODY    2
#define RECV_MAGIC     3                 /* the Wimp itself */

struct wimp_qentry {
    struct wimp_qentry *next;
    uint32_t reason;
    int recv_kind;
    uint32_t recv;                       /* the task handle (RECV_TASK) */
    uint32_t cursor;                     /* the next slot (RECV_BROADCAST) */
    uint32_t sender;                     /* a task handle, or 0 for the Wimp */
    uint32_t window, icon;               /* what it was addressed to, for deletion */
    uint32_t size;
    uint8_t data[256];
};

/* ---- windows -------------------------------------------------------------- */

/* A box in OS units, x1 and y1 exclusive */
struct wimp_box {
    int32_t x0, y0, x1, y1;
};

/* A rectangle list (region.c), disjoint rectangles in order */
#define WIMP_RL_MAX 1024u
struct wimp_rlist {
    uint32_t n;
    uint32_t overflow;                   /* a rectangle could not be kept */
    struct wimp_box b[WIMP_RL_MAX];
};

/* The furniture's sizes, from the tools */
struct wimp_furniture {
    int32_t T, V, H, B, C, I, L, R, U, D;
};

/* A caret block */
struct wimp_caretblk {
    uint32_t w;                          /* window handle, or -1 */
    int32_t i, x, y;                     /* icon (-1 for the work area), then the work area x, y */
    uint32_t hf;                         /* height and flags */
    int32_t index;
};

/* The internal event that hands a writable icon's key to its owner */
#define WIMP_EV_EDIT 0x10000u

/* The iconbar: the records of its two sides */
#define WIMP_IB_MAX 128u
struct ib_rec {
    int32_t icon;                        /* in the iconbar window */
    uint32_t task;                       /* the creator's internal handle */
    int32_t prio, defw;
};
struct ib_side {
    struct ib_rec r[WIMP_IB_MAX];        /* from the screen edge inwards */
    uint32_t n;
    int32_t M, E, W, O;
};
struct wimp_ib {
    struct ib_side side[2];
    int flipped, needs_rs;
};

/* Wimp_ReportError's state (errorbox.c) */
/* commandflag's values */
#define CF_DORMANT 0u
#define CF_PENDING 1u
#define CF_ACTIVE 2u
#define CF_WIMPVDU 0x40u
#define CF_SUSPENDED 0x80u

struct wimp_err {
    uint32_t handle;                     /* the error window, or 0 */
    int open;
    uint32_t commandflag;                /* the command window's: 0 dormant, 1 pending, 2 active, +&80 suspended */
    uint32_t flags, spritearea, buttonlist, iconend, describebuttons;
    int icondata;
    uint32_t cancelstr;
    uint32_t progsave[4];
    uint8_t wderror[248];
    uint8_t copy[240];
    uint8_t title[1024];                 /* the window's indirected data */
    uint8_t oktext[24], conttext[24], quittext[24];
    uint8_t buttons[256];
    int32_t but_y0_def, but_y1_def, but_w_def, but_y0, but_y1, but_w;
    uint32_t but_fl_def, but_va_def, but_fl, but_va;
    int32_t app_x0, type_x0, mess_x0, L;
    uint32_t old_escape, old_mouse, sprite_save[4], redir[2], old_fx3, vdu_status, saved_pending;
    uint32_t msgs[4];
    int msgs_open;
    uint8_t scratch[64];
};

/* The input state (input.c) */
struct wimp_pending {
    int32_t x, y;
    uint32_t buttons, window;
    int32_t icon;
    uint32_t time, flags, wait, rate;
    int clicks;
};
struct wimp_drag {
    uint32_t type, window, task, flags;
    struct wimp_box box, off;            /* the box drawn, and off relative to the visible area */
    int drawn;
    uint32_t r12, draw, remove, move;    /* types 8-11's routines */
    int32_t lx, ly;                      /* the pointer at the last step */
};
struct wimp_input {
    int32_t mx, my;
    uint32_t mb, mt, oldb;
    int reuse;
    int early;                           /* a drag fired early, so no type 15 actions */
    /* pointer shapes */
    uint32_t ptr_w;                      /* old_window, old_icon: the last hit-test seen */
    int32_t ptr_i;
    int special;                         /* special_pointer: a P command's sprite is in use */
    int cnp_write;                       /* cnp_ptr_writable: ptr_write2 was used */
    char ptr_sprite[32];                 /* pointer_sprite: that sprite's name */
    struct wimp_pending pend;
    uint32_t ptrwindow, ptrtask;
    struct wimp_drag d;
    uint8_t dash1, dash2;
    uint32_t drag_time, drag_move, dclick_time, dclick_move;
    int release_furniture;
    uint32_t wimpflags;
    /* a continuous drag's last Open_Window_Request, which is not sent
     * again while the drag proposes the same (#135) */
    uint8_t drag_sent[32];
    int drag_sent_ok;
};

/* Wimp_SetColourMapping's state */
struct wimp_colourmap {
    int palette_kind;                    /* 0: none (the Wimp palette).  1: palette[] */
    uint32_t palette[16];
    uint8_t map1[2], map2[4], map4[16];
};

/* The one autoscroll that Wimp_AutoScroll runs (input.c) */
struct wimp_autoscroll {
    int on, pausing;
    uint32_t flags, win, pause, routine, ws, state;
    int32_t zones[4];
    uint32_t pause_end, last, next;
    int32_t px, py;
};

/* Icon autoscrolling (caret.c) */
struct wimp_iconscroll {
    uint32_t state, win;                 /* IS_ flags, and the window or -1 */
    int32_t icon;                        /* the icon, or -1 */
    uint32_t next;                       /* the next step's time */
    int32_t prevx, min, max;             /* the pointer's last x, and the scroll's limits */
    uint32_t oldptr;                     /* the pointer shape before ptr_autoscrh */
};

/* The Clipboard Manager task (clipboard.c) */
enum { CB_PENDING, CB_RUN, CB_RUNNING, CB_DORMANT };
#define CB_PW_COPY      1u
#define CB_PW_PASTE     2u
#define CB_PW_CUT       3u
#define CB_PW_DRAGSTART 4u
#define CB_PW_DRAGABORT 5u
#define CB_PW_DATALOAD  256u
/* The Clipboard Manager's drag of text out of a selection (CBTask's
 * cbtask_var_*) */
struct wimp_cbdrag {
    int dragging, finished, aborted;     /* our drag is running, has ended, was ended by Escape */
    int claiming, ghost;                 /* as the receiver: a writable icon has claimed, the ghost is shown */
    int shift, delete_source;
    uint32_t claimant, lastref, old_flags;
    uint32_t claimed_w;
    int32_t claimed_i, last_x, last_scrollx;
    uint32_t src_w;                      /* where the dragged selection is */
    int32_t src_i, src_lo, src_hi;
    int32_t box[4], mpt[4], parent[4];   /* the box from the pointer in OS units and millipoints, and its bounds */
};

struct wimp_clip {
    int state;
    uint32_t handle;                     /* its task handle while running */
    uint32_t pollword;
    uint8_t park[256];                   /* the parked DataLoad */
    uint32_t park_size;
    int parked;
    uint32_t paste, paste_cap, paste_len;    /* the data being pasted, an RMA block */
    uint8_t block[256];                  /* its poll block */
    uint8_t text[1024];                  /* an icon's text, fetched */
    uint8_t valid[256];
    uint8_t name[32], title[16];
    uint32_t list[12];
    struct wimp_cbdrag d;
    uint32_t drag, drag_len;             /* the dragged text (dragdata), an RMA block */
    uint8_t pinfo[20];                   /* Wimp_GetPointerInfo's block */
    uint8_t dbox[56];                    /* a Wimp_DragBox block */
    uint8_t ws[40];                      /* Wimp_GetWindowState's */
};

/* A remembered work-area tile: slots 0-15, m, i */
struct wimp_tile {
    int looked;
    uint32_t sprite, area, table;
    uint32_t scale[4];
    uint32_t pix_at, pix_size;           /* its translation table, in the RMA */
};

/* The tool list's slots, in maketoollist's order */
enum {
    TOOL_BACK, TOOL_PBACK, TOOL_CLOSE, TOOL_PCLOSE, TOOL_TOGGLE, TOOL_PTOGGLE, TOOL_TOGGLE1,
    TOOL_PTOGGLE1, TOOL_SIZE, TOOL_PSIZE, TOOL_ICONISE, TOOL_PICONISE, TOOL_UP, TOOL_PUP,
    TOOL_DOWN, TOOL_PDOWN, TOOL_RIGHT, TOOL_PRIGHT, TOOL_LEFT, TOOL_PLEFT, TOOL_TBARLCAP,
    TOOL_PTBARLCAP, TOOL_TBARMIDT, TOOL_PTBARMIDT, TOOL_TBARMIDB, TOOL_PTBARMIDB, TOOL_TBARRCAP,
    TOOL_PTBARRCAP, TOOL_VWELLTCAP, TOOL_VWELLT, TOOL_VBART, TOOL_PVBART, TOOL_VBARMID,
    TOOL_PVBARMID, TOOL_VBARB, TOOL_PVBARB, TOOL_VWELLB, TOOL_VWELLBCAP, TOOL_HWELLLCAP,
    TOOL_HWELLL, TOOL_HBARL, TOOL_PHBARL, TOOL_HBARMID, TOOL_PHBARMID, TOOL_HBARR, TOOL_PHBARR,
    TOOL_HWELLR, TOOL_HWELLRCAP, TOOL_HBLIP, TOOL_PHBLIP, TOOL_VBLIP, TOOL_PVBLIP, TOOL_BLANK,
    TOOL_SLOTS
};

/* The tool list, and the parameters the tools are plotted with */
struct wimp_tools {
    uint32_t list[TOOL_SLOTS];           /* sprite | 1 if masked, or 0 */
    int built;
    int unlisted;                        /* tool_list = 0: never made, and the sizes are 0 */
    int regen;                           /* tsprite_needsregen: make the translation again before use */
    uint32_t master[16], active[16];     /* table_n's image data, or 0 */
    uint32_t scale[4];                   /* xmul, ymul, xdiv, ydiv */
    uint32_t table;                      /* the generated table, or 0 */
    uint32_t action;                     /* 32, or 40 with masks */
    int32_t title_left, title_right, title_section, title_top, title_bottom;
    int32_t vs_top, vs_topfill, vs_blobtop, vs_blobfill, vs_blobbot, vs_botfill, vs_bot;
    int32_t hs_left, hs_leftfill, hs_blobleft, hs_blob, hs_blobright, hs_rightfill, hs_right;
    int32_t hblip_w, vblip_h;
    uint32_t pix_at, pix_size;           /* the tools' table (tpixtable_at), in the RMA */
    uint32_t act_at, act_size;           /* mastertoactive's block (ttt_activeset), in the RMA */
};

/* The icon drawer's state: the clip, the window's and the icon's colours,
 * and the desktop font */
enum { TC_FG, TC_BG, TC_BG2, TC_WELL, TC_FACE, TC_OPP, TC_SELFG, TC_SELBG };
/* *WimpVisualFlags' colours, in its keywords' order */
enum { TH_WBFC, TH_WBOC, TH_MBFC, TH_MBOC, TH_BBFC, TH_BBSC, TH_BBOC, TH_BC, TH_BWC, TH_BHC, TH_WOC, TH_COUNT };
extern const uint32_t wimp_theme_defaults[TH_COUNT];
struct wimp_draw {
    struct wimp_box clip;                /* the graphics window last set */
    const struct wimp_window *win;       /* the window drawn, or NULL */
    uint32_t titlefg, titlebg, workfg, workbg, scout, scin, titlebg2, titlecolour;
    uint32_t area;                       /* areaCBptr: the window's sprite area */
    uint32_t this_area, spritename, lengthflags, validation;
    uint32_t border_type, border_highlight;
    int32_t linespacing;
    uint32_t fontfg, fontbg;
    uint32_t tc[8];
    uint32_t threed;                     /* ThreeDFlags */
    uint32_t theme[TH_COUNT];            /* the theme colours, &BBGGRR00 */
    uint32_t textop_fg, textop_bg;       /* Wimp_TextOp 0's colours */
    int textop_set;
    int counting, line_count;            /* formatted text, counting its lines */
    int scroll_mode;                     /* the text scroll: SC_* */
    int32_t scroll_v, scroll_sx, scroll_sw;
    int sel_on;                          /* the icon drawn holds the window's selection */
    int buffered;                        /* drawn through the buffer (cnp_buffered) */
    int32_t buf_x0, buf_y0;              /* the buffer's place on screen: its sprite's origin */
    int sel_insert;                      /* fontstring to insert the selection's codes */
    int sel_shaded;                      /* ... in a window other than selwin */
    int32_t sel_lo, sel_hi;
    uint32_t sel_codes[2][2];            /* the colour changes at its ends (control 19) */
    /* the desktop font */
    uint32_t systemfont, symbolfont;
    int32_t systemfonty0, systemfonty1, systemfontwidth;
    uint32_t symbol_map;
    int fontnumber;                      /* the walk's n, cached */
    uint32_t currentfont, currentbg, currentfg, currentoffset;
    /* sprites */
    uint32_t sprite_lastmode;
    int32_t sprite_log2bpp, sprite_log2px, sprite_log2py;
    int sprite_needs;
    uint32_t sprite_scale[4];
    uint8_t namebuf[16];
    uint8_t lookup[16];
    uint8_t glyph[4];
    uint8_t fontstr[512];
    uint8_t fontname[256];
    uint8_t pixtable[1024];              /* font.c's scratch */
    uint32_t pix_at, pix_size, pix_used; /* cachespritepixtable's table (pixtable_at), in the RMA */
    uint32_t bbox[9];
    uint32_t inverse_entry, inverse_maps; /* selected and shaded sprites */
};

/* A window's placement is kept in two copies.  REQ is what was requested
 * (by OpenWindow and CloseWindow).  APP is what is applied, which is what
 * the screen shows.  The flush moves one to the other. */
enum { REQ, APP };
#define NO_WINDOW 0xFFFFFFFFu
struct wimp_place {
    int open;
    uint32_t parent;                     /* a handle, or NO_WINDOW */
    uint32_t below;                      /* the next window down its stack, or 0 */
    uint32_t top_child;                  /* the front of its child stack, or 0 */
    struct wimp_box vis, outline;
    int32_t scx, scy;
    uint32_t flags;                      /* the flags word, status bits included */
};

/* A window's selection (w_seldata) */
#define SEL_BIGNUM 0x20000000
struct wimp_sel {
    int32_t icon;                        /* -1 none */
    int32_t xoff, width, yoff;           /* xoff from the icon's text origin */
    uint32_t flags;
    int32_t low, high;
    int32_t xoverride;                   /* held text scroll, or SEL_BIGNUM */
    uint32_t checksum;
};

/* A surface window's binding (surface.c).  The window's work area is a
 * sprite, which the Wimp draws itself, scaled, with no Redraw_Window_
 * Request.  The binding is an RMA block, because the factors and the table
 * are handed to OS_SpriteOp 52. */
#define WS_VDISPLAY     (1u << 0)        /* source is a VDisplay's handle */
#define WS_EXTERNAL     (1u << 1)        /* source is the compositor's id for a Linux
                                            program's window, which it shows there */
#define WIMP_SURFACE_REASON 0x5200u      /* Wimp_Extend &5200-&5203 */
#define MSG_SURFACE_RESIZED 0xC01C2u     /* Message_SurfaceResized, to the owner */
struct wimp_surface {
    uint32_t flags;
    uint32_t source;                     /* a VDisplay's handle, or a sprite area */
    uint32_t sprite;                     /* the sprite (not a VDisplay's) */
    uint32_t scale;                      /* 1-16, or 0: fitted to the visible area */
    struct ros_vdisplay_view view;       /* a VDisplay's, as last seen */
    uint32_t width, height, xeig, yeig, log2bpp;    /* the sprite's, now */
    uint32_t mode_gen;                   /* the display's mode the extent was set for */
    int table_ok;                        /* table[] is for the palette and desktop now */
    uint32_t table_pal_gen, action;      /* OS_SpriteOp 52's R5 */
    uint32_t presents, rects;            /* times shown and rectangles plotted, for measurement */
    int32_t factors[4];
    uint8_t table[1024];
};

struct wimp_window {
    uint32_t handle;
    uint32_t owner;                      /* a task handle, 0 for the Wimp, or NO_WINDOW for a menu */
    uint8_t def[88];                     /* the creation block, as stored */
    uint32_t align;                      /* alignment flags */
    struct wimp_place s[2];              /* REQ and APP */
    int marked;                          /* to be flushed */
    struct wimp_box toggle_vis;          /* the toggle-back state */
    int32_t toggle_scx, toggle_scy;
    uint32_t toggle_behind;
    int32_t shift_toggle_h;
    uint32_t icons;                      /* the icon array (an RMA block), or 0 */
    uint32_t nicons;
    struct wimp_sel sel;
    struct wimp_surface *surface;        /* a surface window's binding, or NULL */
};

#define WIMP_WINDOWS 4096u

/* The menu tree (menus.c): levels 0 to top */
#define WIMP_MENU_LEVELS 8
struct wimp_menus {
    int32_t top;                         /* menuSP / 4, or -1 for none */
    uint32_t handles[WIMP_MENU_LEVELS];  /* each level's window */
    uint32_t data[WIMP_MENU_LEVELS];     /* a menu's first item (block + 28), or a dialogue box */
    int32_t sel[WIMP_MENU_LEVELS];       /* the recorded item, -1 none */
    uint32_t task;                       /* the owner's handle, 0 none */
    uint32_t handle;                     /* menuhandle */
    int temporary, reversed, external;
    int32_t scrolly;
    uint32_t caretwin;                   /* where the menu code put the caret, or -1 */
    int32_t careticon;
    struct wimp_caretblk oldcaret;
    int32_t which;                       /* the level findmenu found, -1 none */
    uint32_t timeout, inactive;          /* the automatic-open and menu-drag deadlines */
    int byclick;
    uint32_t lastw;                      /* the last check's window and icon */
    int32_t lasti;
    uint32_t timelimit, dragdelay;       /* centiseconds */
    int autoopen, clicksub;              /* WimpFlags bit 7, and WimpClickSubmenu */
};

/* ---- the workspace -------------------------------------------------------- */

struct wimp_ws {
    struct wimp_task *task[WIMP_TASKS];  /* RMA blocks, or NULL for an unused slot */
    uint32_t ntasks;                     /* live tasks */
    uint32_t generation;                 /* the next version bits */
    uint32_t my_ref;                     /* the last my_ref given */
    struct wimp_qentry *queue, *queue_tail;
    uint32_t last_sender;                /* R2's record */
    uint32_t null_last;                  /* the slot that last had a null */
    /* the pollword list: slots in list order */
    uint32_t npollwords;
    uint32_t pollwords[WIMP_TASKS];
    /* the mode (screen.c) */
    uint32_t xeig, yeig, log2bpp;
    int32_t dx, dy, screen_w, screen_h;
    uint32_t palette[16];                /* the mapping palette */
    uint32_t others[4];                  /* the border and pointer colours */
    uint8_t scratch[512];                /* arena bytes for SWI arguments */
    /* null-event pacing (task.c) */
    uint64_t pace_wake_ns;               /* the earliest held null's time, 0 if none held */
    uint64_t pace_ns;                    /* the interval, or 0 for off */
    uint32_t pace_boot_ns;               /* rosgd.nullpace's, or the default */
    uint32_t pace_read_cs;               /* when Wimp$NullPace was last read */
    int pace_read;                       /* read at all yet */
    char pace_var[48];                   /* arena bytes: its name and value */
    /* the tools (tools.c) */
    uint32_t tools;                      /* the tool sprite area, or 0 */
    struct wimp_furniture furn;
    struct wimp_tools tl;
    struct wimp_draw dr;
    /* windows (window.c) */
    struct wimp_window *win[WIMP_WINDOWS];
    uint32_t win_generation;
    uint32_t top[2];                     /* the top-level stacks' fronts, REQ and APP */
    uint32_t back_window;                /* the Wimp's own, or 0 */
    uint32_t old_back;                   /* backwindow: an old-style task's, or 0 */
    int32_t poller;                      /* the slot of the task whose poll is searching, or -1 */
    int any_marked;
    int mode_changed;                    /* Wimp_SetMode since the last poll sent a mode change's messages */
    uint32_t mode_copy;                  /* copy_mode_specifier: the Wimp's own copy of a selector */
    int32_t lastmode_w, lastmode_h;      /* the screen at the last Message_ModeChange */
    uint32_t forceflags;                 /* ST_FORCE if the screen has shrunk since */
    int32_t wheel_x, wheel_y;            /* the scroll wheels' totals at the last look (OS_Pointer 2) */
    int wheel_seen;
    uint32_t start_app_space;            /* ReadSysInfo 11 */
    /* Wimp_StartTask and dead tasks (task.c) */
    uint32_t parents[WIMP_TASKS];        /* slots waiting in Wimp_StartTask, last on top */
    uint32_t nparents;
    int32_t singletask;                  /* the single-tasking program's slot, or -1 */
    struct ros_task *reap[16];           /* ended tasks' threads, to free */
    uint32_t nreap;
    uint32_t exit_entry, error_entry;    /* the Wimp's handlers for its children */
    uint8_t errbuf[256];                 /* the error handler's buffer */
    struct ros_task *first_rt;           /* the first task's domain, parked when dead */
    uint32_t saved_exit[3];              /* wimpquithandler: the exit handler it had before */
    uint32_t handlerword;                /* handlers changed since the application started */
    uint32_t parentquit;                 /* the exit handler when it started */
    struct wimp_tile tiles[18];
    struct wimp_err err;
    /* input (input.c) and the iconbar */
    struct wimp_input in;
    uint32_t writedir, mode;             /* *WimpWriteDir, and currentmode: the Wimp's mode, or -1 if unset */
    uint32_t appspacesize;               /* the slot size Message_SlotSize last gave */
    uint32_t statevar;                   /* Wimp$State's code and its answers, in the RMA */
    uint32_t sprout_current, sprout_correct;     /* output's destination, and the one dx and dy are for */
    uint32_t iconbar;                    /* the iconbar window, or 0 */
    struct wimp_ib ib;
    int32_t iconbar_height;
    int32_t iconbar_laid_w;             /* the screen width the bar is laid out for */
    /* the template file (templates.c) */
    uint32_t tfile, tsize;               /* the file, read whole into the RMA, or 0 for none */
    uint32_t tfonts[256];                /* template font n's handle since it was opened */
    /* the caret and keys (caret.c) */
    struct wimp_caretblk caret, ghost, saved;
    uint32_t focus;                      /* the window marked with the focus, or 0 */
    uint32_t selwin;                     /* the window whose selection is drawn unshaded, or -1 */
    uint32_t clipdata, cliplen;          /* the Wimp's own clipboard (an RMA block) */
    int refresh_main;                    /* refreshmaincaret */
    int32_t caretx, ghostcaretx;         /* offsets from their icons' text origins */
    int cnp_drag;                        /* what a type 15 drag does next */
    int32_t ghost_xoverride;             /* the ghost caret's held text scroll, or SEL_BIGNUM */
    int32_t sel_origin;                  /* selection_text_origin: the selection's text, on screen */
    struct wimp_iconscroll is;           /* icon autoscrolling */
    uint32_t hotkeyptr;                  /* the chain's next window, 0 for none, or -1 past the end */
    uint8_t inj[256];                    /* keys from Wimp_ProcessKey */
    uint32_t ninj;
    uint8_t exp[512];                    /* a function key's expansion */
    uint32_t nexp, pexp;
    /* the sprite pool (sprites.c) */
    uint32_t rom_sprites, ram_sprites;   /* sprite areas, 0 for none */
    char rom_name[48];                   /* the ROM sprite file loaded */
    char rom_suffix[4];                  /* romspr_suffix: from the Wimp's mode, "xy" */
    uint8_t empty_area[16];              /* loseromsprites' empty sprite area */
    uint32_t ram_number;                 /* the RAM area's dynamic area */
    /* redraw (redraw.c) */
    struct wimp_rlist invalid;
    struct wimp_rlist pending;           /* the pending redraw or update list */
    uint32_t pending_window;             /* 0 for none */
    int pending_update;                  /* set for an update, clear for a redraw */
    int pending_first;                   /* no rectangle handed out yet */
    struct wimp_box pending_rect;        /* the rectangle last handed out */
    struct wimp_menus mn;                /* menus (menus.c) */
    struct wimp_clip clip;               /* the Clipboard Manager (clipboard.c) */
    uint32_t ext_jump[8];                /* Wimp_Extend 2's table */
    uint32_t ext_name, ext_ok;           /* Wimp_Extend 4's words */
    uint32_t claim_area, claim_base;     /* Wimp_ClaimFreeMemory's area */
    int claim_lent;
    struct wimp_colourmap cmap;
    struct wimp_autoscroll as;
    uint32_t save_context;               /* a VDU save area for the screen */
    int watchdog_off;                    /* Wimp_SetWatchdogState */
    uint32_t watchdog_word;
    uint32_t cmd_handle;                 /* the command window (cmdwin.c) */
    uint8_t cmd_title[256];
    uint8_t cmd_vdu[32];
    uint8_t oldfx[13];                   /* the key settings before the desktop */
    /* surface windows (surface.c) */
    uint32_t nsurfaces;                  /* windows bound */
    int surf_dirty;                      /* a VDisplay changed: show it at a safe point */
    int surf_desk_gen;                   /* the desktop's mode or palette, for the tables */
};

extern struct ros_module wimp_module;
struct wimp_ws *wimp_ws(void);

/* The task table (task.c) */
struct wimp_task *wimp_current(void);
struct wimp_task *wimp_task_by_handle(uint32_t handle, int live_match);
uint32_t wimp_slot_of(const struct wimp_task *t);
void wimp_task_thread_ended(struct ros_task *rt);

/* The message queue (message.c) */
os_error *wimp_queue_message(uint32_t reason, const uint8_t *block, uint32_t size,
                             int recv_kind, uint32_t recv, uint32_t sender);
int wimp_queue_next(struct wimp_task **to, struct wimp_event *ev);
void wimp_queue_forget_task(uint32_t handle);
void wimp_queue_forget_window(uint32_t handle);
void wimp_queue_discard(void);
int wimp_accepts(const struct wimp_task *t, uint32_t action);
int wimp_queue_update_slotsize(uint32_t sender, uint32_t current, uint32_t next);

/* Rectangle lists (region.c) */
struct wimp_rlist *wimp_rl_new(void);
void wimp_rl_free(struct wimp_rlist *l);
void wimp_rl_copy(struct wimp_rlist *to, const struct wimp_rlist *from);
void wimp_rl_append(struct wimp_rlist *l, struct wimp_box r);
void wimp_rl_clip(struct wimp_rlist *l, struct wimp_box c);
void wimp_rl_subtract(struct wimp_rlist *l, struct wimp_box c);
void wimp_rl_add(struct wimp_rlist *l, struct wimp_box c);
void wimp_rl_minus(struct wimp_rlist *l, const struct wimp_rlist *m);
void wimp_rl_and(struct wimp_rlist *l, const struct wimp_rlist *m);
void wimp_rl_union(struct wimp_rlist *l, const struct wimp_rlist *m);
void wimp_rl_translate(struct wimp_rlist *l, int32_t dx, int32_t dy);
int wimp_rl_meets(const struct wimp_rlist *l, struct wimp_box c);

/* The mode, colours and the Wimp's own drawing (screen.c) */
extern const uint32_t wimp_default_palette[16];
extern const uint32_t wimp_default_others[4];
void wimp_mode_refresh(void);
struct wimp_box wimp_screen_box(void);
int32_t wimp_round_x(int32_t x);
int32_t wimp_round_y(int32_t y);
struct wimp_box wimp_round_box(struct wimp_box b);
void wimp_vdu5(void);
void wimp_vdu_bytes(const uint8_t *bytes, uint32_t n);
void wimp_graphics_window(struct wimp_box r);
void wimp_default_windows(void);
void wimp_clg(void);
void wimp_ecf_origin(int32_t x, int32_t y);
void wimp_gcol(uint32_t colour, int background);
uint32_t wimp_colour(uint32_t n);
void wimp_copy_rect(struct wimp_box src, int32_t dx, int32_t dy);

/* The sprite pool (sprites.c) */
void wimp_sprites_start(void);
uint32_t wimp_pool_find(uint32_t name, uint32_t *area);
void wimp_swi_SpriteOp(struct ros_cpu *s);
void wimp_swi_BaseOfSprites(struct ros_cpu *s);
os_error *wimp_cmd_iconsprites(struct ros_module *m, uint32_t tail, uint32_t argc);

/* The tools (tools.c) */
void wimp_tools_unlist(void);
void wimp_sprites_choose(void);
void wimp_sprites_lose(void);
void wimp_read_mode(void);
uint32_t wimp_table_space(uint32_t *at, uint32_t *have, uint32_t size);
os_error *wimp_int_setmode(uint32_t mode);
void wimp_sprout_recache(void);
void wimp_slot_moved(uint32_t size);
void wimp_tools_refresh(void);
uint32_t wimp_tool_table(uint32_t colour);

/* The desktop font (font.c) */
void wimp_find_font(void);
void wimp_font_exit(void);
uint32_t wimp_measure_symbols(uint32_t handle);

/* Windows (window.c) */
struct wimp_window *wimp_window(uint32_t handle);
void wimp_windows_start(void);
void wimp_windows_end(void);
void wimp_mode_change_requests(void);
void wimp_windows_of_task_delete(uint32_t task);
struct wimp_window *wimp_window_below(const struct wimp_window *w, int st);
struct wimp_window *wimp_stack_front(uint32_t parent, int st);
void wimp_visible(const struct wimp_window *w, struct wimp_box box, int st, struct wimp_rlist *out);
void wimp_inner(const struct wimp_window *w, int st, struct wimp_rlist *out);
int32_t wimp_origin_x(const struct wimp_window *w, int st);
int32_t wimp_origin_y(const struct wimp_window *w, int st);

/* Icons (icons.c) and drawing them (draw.c) */
uint8_t *wimp_icon(const struct wimp_window *win, uint32_t i);
os_error *wimp_icons_from_block(struct wimp_window *win, uint32_t block, uint32_t n);
void wimp_icons_free(struct wimp_window *win);
void wimp_draw_icons(struct wimp_window *win, struct wimp_box rect);
void wimp_draw_border(struct wimp_window *win, const struct wimp_rlist *rects);
void wimp_draw_3d_border(struct wimp_window *win);
void wimp_vdu_init(void);
void wimp_set16x32(void);
int wimp_count_lines(const struct wimp_window *win, uint32_t flags, uint32_t text, struct wimp_box b);
int32_t wimp_icon_text_width(const struct wimp_window *win, uint32_t i);
int wimp_draw_tile(const struct wimp_window *win);
void wimp_tiles_forget(void);

/* The caret, keys and writable icons (caret.c, with placing and drawing in draw.c) */
void wimp_caret_reset(void);
void wimp_caret_window_gone(uint32_t handle);
void wimp_caret_paint(const struct wimp_window *win);
int wimp_key_event(struct wimp_task **to, struct wimp_event *ev);
int wimp_edit_key(uint32_t code, struct wimp_event *ev);
/* What a measurement computes (caretx_dest) */
enum { CC_NONE, CC_MAIN, CC_GHOST, CC_SEL_LOW, CC_SEL_HIGH };
int wimp_caret_coords(struct wimp_window *win, int32_t icon, uint32_t index, int dest, int32_t *x,
                      int32_t *y, uint32_t *hword, int32_t *cx);
int wimp_caret_find(struct wimp_window *win, int32_t icon, int32_t px, int32_t py, int dest, int32_t *x,
                    int32_t *y, uint32_t *hword, uint32_t *index, int32_t *cx);
int wimp_sel_hold(struct wimp_window *win, int32_t icon, int ghost, int32_t *tw, int32_t *iw, uint32_t *flags,
                  int32_t *hold);
uint32_t wimp_icon_checksum(struct wimp_window *win, int32_t icon);
int wimp_sel_valid(struct wimp_window *win);
int wimp_icon_in_place(const struct wimp_window *win, uint32_t flags);
void wimp_caret_draw(const struct wimp_caretblk *c, int32_t x, int32_t y);
void wimp_swi_SetCaretPosition(struct ros_cpu *s);
void wimp_swi_GetCaretPosition(struct ros_cpu *s);
void wimp_swi_ProcessKey(struct ros_cpu *s);

/* Input (input.c) */
void wimp_input_config(void);
void wimp_input_window_gone(uint32_t handle);
void wimp_input_mode_set(void);
void wimp_input_icon_created(struct wimp_window *win, uint32_t block);
int wimp_input_event(struct wimp_task **to, struct wimp_event *ev);   /* 2 null, 3 nothing */
struct wimp_window *wimp_hit(int32_t x, int32_t y, int shaded, int32_t *icon);
int wimp_wheel(void);
uint32_t wimp_old_back_window(uint32_t owner);
int32_t wimp_gadget(const struct wimp_window *win, int32_t x, int32_t y);
uint32_t wimp_reported(const struct wimp_window *win);
void wimp_drag_hide(void);
void wimp_drag_show(void);
void wimp_drag_cancel(void);
void wimp_swi_GetPointerInfo(struct ros_cpu *s);
void wimp_swi_DragBox(struct ros_cpu *s);
void wimp_swi_AutoScroll(struct ros_cpu *s);
int32_t wimp_iconbar_owner(int32_t icon);
os_error *wimp_iconbar_create(uint32_t form, uint32_t r0, uint32_t block, struct wimp_task *t, uint32_t *handle);
void wimp_iconbar_delete(int32_t icon);
void wimp_iconbar_task_gone(uint32_t internal);
uint32_t wimp_iconbar_icon_task(int32_t icon);
void wimp_iconbar_refit(void);
void wimp_iconbar_layout(int side);
void wimp_iconbar_mode(void);
int wimp_template_load(const char *name, uint8_t out[88]);
struct wimp_window *wimp_create_system(const uint8_t *block, os_error **err);
struct wimp_window *wimp_create_system_icons(const uint8_t *block, os_error **err);
void wimp_delete_system(struct wimp_window *win);
struct wimp_window *wimp_window_ib(uint32_t handle);
os_error *wimp_icon_add(struct wimp_window *win, const uint8_t *block, uint32_t *slot);
void wimp_icon_remove(struct wimp_window *win, uint32_t i);
void wimp_swi_TextOp(struct ros_cpu *s);
void wimp_swi_PlotIcon(struct ros_cpu *s);
void wimp_swi_ReadPixTrans(struct ros_cpu *s);
void wimp_swi_SetColourMapping(struct ros_cpu *s);
void wimp_colourmap_reset(void);
void wimp_cmdwin_make(void);
os_error *wimp_command_window(uint32_t r0);
void wimp_command_pending(uint32_t command);
void wimp_keys_restore(void);
void wimp_cmdwin_mode_changing(void);
void wimp_swi_CommandWindow(struct ros_cpu *s);
void wimp_swi_SetMode(struct ros_cpu *s);
void wimp_swi_SetPointerShape(struct ros_cpu *s);
void wimp_swi_SetWatchdogState(struct ros_cpu *s);
void wimp_start(struct ros_module *m, uint32_t tail);
os_error *wimp_cmd_wimptask(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *wimp_cmd_pointer(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *wimp_cmd_wimpwritedir(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *wimp_cmd_wimpkillsprite(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *wimp_cmd_wimpvisualflags(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *wimp_cmd_toolsprites(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *wimp_cmd_wimpmode(struct ros_module *m, uint32_t tail, uint32_t argc);
void wimp_swi_ReportError(struct ros_cpu *s);
void wimp_errorbox_make(void);
void wimp_redraw_now(struct wimp_window *win);
void wimp_close_system(struct wimp_window *win);
void wimp_scroll_geom(const struct wimp_window *win, int vertical, struct wimp_box *bar,
                      struct wimp_box *well, struct wimp_box *sausage);
int32_t wimp_muldiv(int32_t a, int32_t b, int32_t c);
uint32_t wimp_behind(const struct wimp_window *win, int st);
void wimp_min_size(const struct wimp_window *win, int32_t *w, int32_t *h);
void wimp_open_own(struct wimp_window *win, uint8_t *block);

/* Menus (menus.c) */
struct wimp_window *wimp_create_menu_window(const uint8_t *block, os_error **err);
void wimp_window_set_flags(struct wimp_window *win, uint32_t bits);
void wimp_open_forced(struct wimp_window *win, uint8_t *block);
void wimp_delete_menu_window(struct wimp_window *win);
os_error *wimp_icons_set(struct wimp_window *win, const uint8_t *blocks, uint32_t n);
uint32_t wimp_font_string(uint32_t flags, uint32_t text);
uint32_t wimp_message(const char *token);
void wimp_caret_set(const struct wimp_caretblk *c);
void wimp_caret_nocaret(uint32_t handle);
void wimp_menus_start(void);
void wimp_menu_config(void);
int32_t wimp_menu_arrow_width(void);
int32_t wimp_menu_level(uint32_t handle);
struct wimp_task *wimp_menu_owner(void);
int wimp_menu_escape(void);
int wimp_menu_return(struct wimp_window *win, int32_t icon, struct wimp_event *ev);
void wimp_menu_poll_entry(void);
void wimp_menu_task_gone(uint32_t task);
void wimp_menu_pointer(struct wimp_window *win, int32_t icon, int nodrag);
int wimp_menu_scan(struct wimp_window *win, int32_t icon, int *suppress, struct wimp_task **to,
                   struct wimp_event *ev);
void wimp_menu_separators(struct wimp_window *win);
const void *wimp_menu_page_in(void);
void wimp_menu_page_back(const void *was);
void wimp_swi_CreateMenu(struct ros_cpu *s);
void wimp_swi_CreateSubMenu(struct ros_cpu *s);
void wimp_swi_DecodeMenu(struct ros_cpu *s);
void wimp_swi_GetMenuState(struct ros_cpu *s);

/* The Clipboard Manager (clipboard.c) */
void wimp_clipboard_reset(void);
int wimp_clipboard_running(uint32_t *handle);
uint32_t wimp_clipboard_title(void);
int wimp_clipboard_intercept(const uint8_t *b, uint32_t size, uint32_t *dest);
void wimp_clipboard_park(uint32_t code, const uint8_t *b, uint32_t size, uint32_t receiver, uint32_t sender);
void wimp_clipboard_unpark(void);
void wimp_clipboard_poll_entry(void);
void wimp_clipboard_request(uint32_t what);
void wimp_clipboard_main(void);
int wimp_valid_chars(uint32_t validation, const uint8_t *p, uint32_t n);
uint32_t wimp_paste_text(struct wimp_window *win, int32_t icon, uint8_t *t, uint32_t cap, uint32_t u,
                         uint32_t index, const uint8_t *data, uint32_t n, uint32_t *start);
void wimp_clip_forget(void);
void wimp_cnp_click(struct wimp_window *win, int32_t icon, uint32_t buttons, int32_t mx, int32_t my,
                    int clicks);
void wimp_cnp_release(struct wimp_window *win, int32_t icon, int clicks);
int wimp_cnp_drag_step(int32_t mx, int32_t my, uint32_t mb);
void wimp_cnp_drag_end(void);
void wimp_iconscroll_start(int selection);
void wimp_iconscroll_stop(void);
void wimp_iconscroll_poll(void);
void wimp_drag_selection(void);
void wimp_drag_selection_cancel(void);
void wimp_iconscroll_swi(struct ros_cpu *s);
int wimp_clipboard_escape(void);
void wimp_clipboard_sel_changed(uint32_t window);
void wimp_clipboard_going(uint32_t window, int32_t icon);
uint32_t wimp_valid_ulimit(uint32_t validation);

/* Filters (filters.c) */
extern struct ros_module filtermgr_native_module;
uint32_t wimp_filter_pre(uint32_t mask, uint32_t block, uint32_t task);
uint32_t wimp_filter_post(uint32_t reason, uint32_t block, uint32_t task);
void wimp_filter_rect(uint32_t window, uint32_t task, struct wimp_box r);
void wimp_filter_postrect(uint32_t window, uint32_t task, struct wimp_box r);
void wimp_filter_posticon(uint32_t window, uint32_t task, struct wimp_box r);
void wimp_filter_copy(uint32_t window, struct wimp_box dst, struct wimp_box src, int32_t dx, int32_t dy);
void wimp_swi_RegisterFilter(struct ros_cpu *s);

/* Surface windows (surface.c) */
void wimp_surface_start(void);
void wimp_extend_surface(struct ros_cpu *s);
void wimp_surface_gone(struct wimp_window *win);
void wimp_surface_paint(struct wimp_window *win, struct wimp_box r);
void wimp_surface_poll(void);
void wimp_surface_desktop_changed(void);
void wimp_mouse_palette(int border);
void wimp_set_extent(struct wimp_window *win, const void *box);

/* Redraw (redraw.c) */
void wimp_icon_changed(struct wimp_window *win, uint32_t i, int in_place);
void wimp_flush(void);
void wimp_invalidate(const struct wimp_rlist *l);
void wimp_invalidate_box(struct wimp_box b);
void wimp_cancel_redraw(void);
int wimp_redraw_scan(struct wimp_task **to, struct wimp_event *ev);

/* The SWIs, as register-level thunks */
void wimp_swi_Initialise(struct ros_cpu *s);
void wimp_swi_CloseDown(struct ros_cpu *s);
void wimp_swi_Poll(struct ros_cpu *s);
void wimp_swi_PollIdle(struct ros_cpu *s);
void wimp_swi_SendMessage(struct ros_cpu *s);
void wimp_swi_AddMessages(struct ros_cpu *s);
void wimp_swi_RemoveMessages(struct ros_cpu *s);
void wimp_swi_SlotSize(struct ros_cpu *s);
void wimp_extend_read_slot_size(struct ros_cpu *s);
void wimp_swi_StartTask(struct ros_cpu *s);
void wimp_swi_ClaimFreeMemory(struct ros_cpu *s);
void wimp_swi_TransferBlock(struct ros_cpu *s);
void wimp_swi_OpenTemplate(struct ros_cpu *s);
void wimp_swi_CloseTemplate(struct ros_cpu *s);
void wimp_swi_LoadTemplate(struct ros_cpu *s);
void wimp_swi_CreateWindow(struct ros_cpu *s);
void wimp_swi_DeleteWindow(struct ros_cpu *s);
void wimp_swi_OpenWindow(struct ros_cpu *s);
void wimp_swi_CloseWindow(struct ros_cpu *s);
void wimp_swi_GetWindowState(struct ros_cpu *s);
void wimp_swi_GetWindowInfo(struct ros_cpu *s);
void wimp_swi_GetWindowOutline(struct ros_cpu *s);
void wimp_swi_SetExtent(struct ros_cpu *s);
void wimp_swi_RedrawWindow(struct ros_cpu *s);
void wimp_swi_UpdateWindow(struct ros_cpu *s);
void wimp_swi_GetRectangle(struct ros_cpu *s);
void wimp_swi_ForceRedraw(struct ros_cpu *s);
void wimp_swi_BlockCopy(struct ros_cpu *s);
void wimp_swi_CreateIcon(struct ros_cpu *s);
void wimp_swi_DeleteIcon(struct ros_cpu *s);
void wimp_swi_SetIconState(struct ros_cpu *s);
void wimp_swi_GetIconState(struct ros_cpu *s);
void wimp_swi_WhichIcon(struct ros_cpu *s);
void wimp_swi_ResizeIcon(struct ros_cpu *s);
void wimp_swi_SetColour(struct ros_cpu *s);
void wimp_swi_TextColour(struct ros_cpu *s);
void wimp_swi_SetFontColours(struct ros_cpu *s);
void wimp_swi_SetPalette(struct ros_cpu *s);
void wimp_swi_ReadPalette(struct ros_cpu *s);

void wimp_fail(struct ros_cpu *s, os_error *e);

/* *Commands */
os_error *wimp_cmd_wimpstats(struct ros_module *m, uint32_t tail, uint32_t argc);
os_error *wimp_cmd_wimpslot(struct ros_module *m, uint32_t tail, uint32_t argc);

#endif
