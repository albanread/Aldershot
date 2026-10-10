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
 * This file is a reimplementation in C of RISC OS Open's source
 * (Sources/Printing/Modules/PDriver: s.PDriver, s.MsgCode, hdr.PDriver).
 */

/* pdriver.c -- PDriver: RISC OS's printer driver, whose pages come out as
 * PDF.
 *
 * Written from the PRM's printing chapter (PRM3 from 3-611, all thirty
 * SWIs) and ROOL's PDriver 3.41 sources, for the behaviour an application
 * can see (Printing/Modules/PDriver, Apache 2.0).  From those sources come
 * its errors, its job list and what SelectJob 0 does.  The driver types
 * come from hdr/PDriverReg.
 *
 * On RISC OS this interface is two modules.  The PDriver "sharer" is what
 * applications call.  A driver (PDriverDP, PDriverPS) renders the page.
 * The drivers are not in ROOL's sources, and the box has one destination.
 * So this module is both.  The SWIs are RISC OS's, and the one driver it
 * enumerates writes PDF.
 *
 * How a page is made:
 *
 *   - The application gives one or more rectangles of its own workspace
 *     (PDriver_GiveRectangle).  Each has a transformation and the place on
 *     the page where its bottom left corner goes.
 *   - This module hands each rectangle back in bands (PDriver_DrawPage,
 *     PDriver_GetRectangle).  VDU output is switched into a sprite the
 *     size of the band (OS_SpriteOp 60, runtime/vdu/sprout.c), with one
 *     pixel to the OS unit.  The application plots in its own coordinates,
 *     and no scaling is needed here.
 *   - Each finished band's pixels go to pdfsvc, the 64-bit PDF writer
 *     beneath RISC OS (../pdfsvc), with the matrix that places it.  The
 *     matrix is the application's transformation, scaled from OS units to
 *     points, so an arbitrary transformation costs nothing.
 *   - At PDriver_EndJob pdfsvc's PDF is written to the file handle the
 *     application opened, which is where RISC OS expects a job's output.
 *
 * The page is pixels, not vectors, at 180 dpi.  A vector page is not made.
 * PDriver_ScreenDump, illustrations and the driver-private SWIs refuse.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "pdriver.h"

/* ---- the errors ------------------------------------------------------------ */

#define E_NOJOB       (PDRIVER_ERRBASE + 0)
#define E_BADJOB      (PDRIVER_ERRBASE + 1)
#define E_INJOB       (PDRIVER_ERRBASE + 2)
#define E_NOROOM      (PDRIVER_ERRBASE + 3)
#define E_TOOMANY     (PDRIVER_ERRBASE + 4)
#define E_SERVICE     (PDRIVER_ERRBASE + 5)
#define E_NOTHERE     (PDRIVER_ERRBASE + 6)
#define E_CANCELLED   (PDRIVER_ERRBASE + 7)
#define E_FEATURES    (PDRIVER_ERRBASE + 8)

/* ---- the page, and the driver ---------------------------------------------- */

/* A4, and a 10 mm margin all round, in millipoints (1/72000 inch) */
#define A4_X        595276u
#define A4_Y        841890u
#define MARGIN       28346u

#define MAX_RECTS   16u                 /* rectangles given for one page */
#define BAND_ROWS  256                  /* a band's height, in pixels */
#define MAX_BAND_W 8192                 /* a band wider than this is refused */

/* 32 bpp, 180 dpi each way: one pixel to the OS unit (runtime/vdu/sprite.c,
 * "type << 27 | ydpi << 14 | xdpi << 1 | 1") */
#define BAND_MODE ((6u << 27) | (PDRIVER_DPI << 14) | (PDRIVER_DPI << 1) | 1u)

/* The features PDriver_Info gives (PRM 3-611 and hdr/PDriver).  They are
 * colour; filled shapes, thick lines and overwriting all possible (those
 * bits mean "cannot" when set); transformed sprites and fonts; DrawPage's
 * flags byte; arbitrary transformations (the page places each band by its
 * matrix); MiscOp; and DeclareFont. */
#define FEATURES ((1u << 0) | (1u << 11) | (1u << 12) | (1u << 13) | \
                  (1u << 25) | (1u << 27) | (1u << 29))

struct give {
    uint32_t id;                        /* the application's word for it */
    int32_t  r[4];                      /* the rectangle, in OS units */
    int32_t  m[4];                      /* the transformation, 16.16 */
    int32_t  pos[2];                    /* where its corner goes, millipoints */
    uint32_t bg;                        /* the background, &BBGGRR00 */
};

struct job {
    struct job *next;
    uint32_t handle;                    /* the file the job is written to */
    char     title[64];
    uint32_t paper_x, paper_y;          /* the page, as it was at SelectJob */
    uint32_t area[4];                   /* its printable area: l, b, r, t */
    int      cancelled;
    uint32_t err_num;                   /* a cancelled job's error */
    char     err_text[64];

    pid_t    pid;                       /* pdfsvc, once it is wanted */
    int      in_fd, out_fd;
    int      page_open;
    unsigned pages;

    struct give give[MAX_RECTS];
    unsigned ngive;
    unsigned cur;                       /* the rectangle being plotted */
    int32_t  band_y;                    /* its band's bottom, in OS units */
    int      drawing;                   /* output is switched into the band */
    int      suspended;                 /* deselected in the middle of a band */
    uint32_t saved[4];                  /* where output went before */
    int32_t  band[4];                   /* the band, in the rectangle's units */
    uint8_t *img;                       /* the rectangle's pixels, its bands so far */
    uint32_t img_w, img_h;
};

static struct job *jobs;
static struct job *current;             /* the selected job, or NULL */
static uint32_t paper_x = A4_X, paper_y = A4_Y;
static uint32_t area[4] = { MARGIN, MARGIN, A4_X - MARGIN, A4_Y - MARGIN };
static uint32_t printer_number;         /* PDriver_SetInfo's R7 */
static int      driver_selected = 1;    /* ours is selected unless told not to */
static char     printer_name[64] = "PDF";
static int      printer_named;          /* PDriver_SetInfo gave a name */

/* the band's sprite, one area reused by every job */
static void    *sp_block;
static uint32_t sp_cap;
static uint32_t sp_area;                /* the sprite area's arena address */
static uint32_t sp_sprite;
static uint32_t sp_w, sp_h;

uint32_t pdriver_paper_x(void) { return current ? current->paper_x : paper_x; }
uint32_t pdriver_paper_y(void) { return current ? current->paper_y : paper_y; }

static os_error *err(uint32_t n, const char *text)
{
    return ros_error(n, "%s", text);
}

/* ---- pdfsvc, the writer ----------------------------------------------------- */

/* Its path: PDFSvc$Exe, else the box's own.  The hosted build has no
 * initramfs, so the Makefile names the one it built. */
static const char *pdfsvc_path(char *buf, size_t max)
{
    char *b = ros_rma_alloc(600);
    if (b) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = ros_addr(b + 300), s.r[1] = ros_addr(b), s.r[2] = 299, s.r[3] = 0, s.r[4] = 3;
        memcpy(b + 300, "PDFSvc$Exe", 11);
        ros_swi(&s, XOS_ReadVarVal);
        if (!s.v && s.r[2] > 0 && (uint32_t)s.r[2] < 299) {
            memcpy(buf, b, s.r[2]);
            buf[s.r[2]] = 0;
            ros_rma_free(b);
            return buf;
        }
        ros_rma_free(b);
    }
#ifdef PDRIVER_PDFSVC
    snprintf(buf, max, "%s", PDRIVER_PDFSVC);
#else
    snprintf(buf, max, "/usr/bin/pdfsvc");
#endif
    return buf;
}

static void service_stop(struct job *j, int kill_it)
{
    if (j->in_fd >= 0)
        close(j->in_fd), j->in_fd = -1;
    if (j->out_fd >= 0)
        close(j->out_fd), j->out_fd = -1;
    if (j->pid > 0) {
        int st;
        if (kill_it)
            kill(j->pid, SIGKILL);
        while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR)
            ;
        j->pid = 0;
    }
    j->page_open = 0;
}

static os_error *service_start(struct job *j)
{
    if (j->pid > 0)
        return NULL;
    char path[320];
    pdfsvc_path(path, sizeof path);

    int to[2], from[2];
    if (pipe(to) < 0)
        return err(E_SERVICE, "The PDF writer could not be started");
    if (pipe(from) < 0) {
        close(to[0]), close(to[1]);
        return err(E_SERVICE, "The PDF writer could not be started");
    }
    /* posix_spawn, not fork: the box is a process of many threads, and a
     * forked child of one may not run far enough to reach execv */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0);
    posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_addclose(&fa, to[0]);
    posix_spawn_file_actions_addclose(&fa, to[1]);
    posix_spawn_file_actions_addclose(&fa, from[0]);
    posix_spawn_file_actions_addclose(&fa, from[1]);
    char *argv[2] = { path, NULL };
    extern char **environ;
    pid_t pid = 0;
    int rc = posix_spawn(&pid, path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        close(to[0]), close(to[1]), close(from[0]), close(from[1]);
        return ros_error(E_SERVICE, "The PDF writer could not be started: %s", strerror(rc));
    }
    close(to[0]), close(from[1]);
    j->pid = pid, j->in_fd = to[1], j->out_fd = from[0];
    j->pages = 0;

    char head[160];
    int n = snprintf(head, sizeof head, "pdf 1\n");
    if (write(j->in_fd, head, (size_t)n) != n) {
        service_stop(j, 1);
        return err(E_SERVICE, "The PDF writer stopped");
    }
    /* a printer that takes no PDF is sent raster instead, at its own
     * resolution: the writer makes it from the same pages */
    uint32_t dpi = 0;
    int gray = 0;
    if (pdriver_dest_format(&dpi, &gray)) {
        n = snprintf(head, sizeof head, "format pwg %u %s\n", dpi, gray ? "sgray" : "srgb");
        if (write(j->in_fd, head, (size_t)n) != n) {
            service_stop(j, 1);
            return err(E_SERVICE, "The PDF writer stopped");
        }
    }
    if (j->title[0]) {
        n = snprintf(head, sizeof head, "title %s\n", j->title);
        if (write(j->in_fd, head, (size_t)n) != n) {
            service_stop(j, 1);
            return err(E_SERVICE, "The PDF writer stopped");
        }
    }
    return NULL;
}

static os_error *service_write(struct job *j, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        ssize_t w = write(j->in_fd, b, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            service_stop(j, 1);
            return err(E_SERVICE, "The PDF writer stopped");
        }
        b += w, n -= (size_t)w;
    }
    return NULL;
}

static os_error *service_say(struct job *j, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static os_error *service_say(struct job *j, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line)
        return err(E_SERVICE, "The page could not be described");
    return service_write(j, line, (size_t)n);
}

/* The job's PDF, from pdfsvc into the file the application opened. */
static os_error *service_finish(struct job *j)
{
    if (j->pid <= 0)
        return NULL;
    os_error *e = service_write(j, "end\n", 4);
    if (e)
        return e;
    close(j->in_fd), j->in_fd = -1;

    uint32_t cap = 64u * 1024u;
    void *buf = ros_rma_alloc(cap);
    if (!buf) {
        service_stop(j, 1);
        return err(E_NOROOM, "There is not enough memory to print");
    }
    uint64_t total = 0;
    for (;;) {
        ssize_t got = read(j->out_fd, buf, cap);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            ros_rma_free(buf);
            service_stop(j, 1);
            return err(E_SERVICE, "The PDF writer stopped");
        }
        if (got == 0)
            break;
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 2, s.r[1] = j->handle, s.r[2] = ros_addr(buf), s.r[3] = (uint32_t)got;
        ros_swi(&s, XOS_GBPB);
        if (s.v) {
            ros_rma_free(buf);
            service_stop(j, 1);
            return err(E_SERVICE, "The print job could not be written");
        }
        total += (uint64_t)got;
    }
    ros_rma_free(buf);

    int st = 0;
    if (j->pid > 0) {
        while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR)
            ;
        j->pid = 0;
    }
    close(j->out_fd), j->out_fd = -1;
    if (total == 0 || (WIFEXITED(st) && WEXITSTATUS(st) != 0))
        return ros_error(E_SERVICE, "The PDF writer could not write the job (%s, %u byte%s)",
                         WIFEXITED(st) ? (WEXITSTATUS(st) == 127 ? "not found" : "it refused")
                                       : (WIFSIGNALED(st) ? strsignal(WTERMSIG(st)) : "it died"),
                         (unsigned)total, total == 1 ? "" : "s");
    return NULL;
}

/* ---- the band's sprite ------------------------------------------------------ */

static os_error *band_sprite(uint32_t w, uint32_t h)
{
    uint32_t size = 16 + 44 + w * h * 4;        /* area header, sprite, pixels */
    if (size > sp_cap) {
        void *p = ros_rma_alloc(size + (size >> 2));
        if (!p)
            return err(E_NOROOM, "There is not enough memory to print");
        if (sp_block)
            ros_rma_free(sp_block);
        sp_block = p, sp_cap = size + (size >> 2);
    }
    sp_area = ros_addr(sp_block);
    sp_sprite = sp_area + 16;
    sp_w = w, sp_h = h;
    ros_st32(sp_area + 0, sp_cap);              /* saEnd */
    ros_st32(sp_area + 4, 1);                   /* saNumber */
    ros_st32(sp_area + 8, 16);                  /* saFirst */
    ros_st32(sp_area + 12, 16 + 44 + w * h * 4);/* saFree */
    uint32_t sp = sp_sprite;
    ros_st32(sp + 0, 44 + w * h * 4);           /* spNext */
    ros_st32(sp + 4, 0x6E616270u);              /* the name, "pband" */
    ros_st32(sp + 8, 0x00000064u), ros_st32(sp + 12, 0);
    ros_st32(sp + 16, w - 1);                   /* spWidth, in words - 1 */
    ros_st32(sp + 20, h - 1);                   /* spHeight - 1 */
    ros_st32(sp + 24, 0);                       /* spLBit */
    ros_st32(sp + 28, 31);                      /* spRBit */
    ros_st32(sp + 32, 44), ros_st32(sp + 36, 44); /* spImage, spTrans */
    ros_st32(sp + 40, BAND_MODE);
    return NULL;
}

/* VDU 29, x; y; sets the origin, so that the application's own
 * coordinates land in the band */
static void set_origin(int32_t x, int32_t y)
{
    uint8_t b[5] = { 29, (uint8_t)(x & 0xFF), (uint8_t)((x >> 8) & 0xFF),
                     (uint8_t)(y & 0xFF), (uint8_t)((y >> 8) & 0xFF) };
    void *p = ros_rma_alloc(sizeof b);
    if (!p)
        return;
    memcpy(p, b, sizeof b);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(p), s.r[1] = sizeof b;
    ros_swi(&s, XOS_WriteN);
    ros_rma_free(p);
}

static void vdu(uint32_t c)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = c;
    ros_swi(&s, XOS_WriteC);
}

/* Service_Print: R2 -1 while a band is drawn, so that the Font Manager hands
 * Font_Paint here (font_paint, below), and 0 otherwise */
static void service_print(uint32_t on)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[1] = 0x41, s.r[2] = on;
    ros_swi(&s, XOS_ServiceCall);
}

/* Output into the band, cleared to the rectangle's background colour. */
static os_error *band_begin(struct job *j, const struct give *g)
{
    uint32_t w = (uint32_t)(j->band[2] - j->band[0]);
    uint32_t h = (uint32_t)(j->band[3] - j->band[1]);
    os_error *e = band_sprite(w, h);
    if (e)
        return e;

    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 60 + 512, s.r[1] = sp_area, s.r[2] = sp_sprite, s.r[3] = 0;
    ros_swi(&s, XOS_SpriteOp);
    if (s.v)
        return err(E_SERVICE, "Output could not be switched to the page");
    if (!j->drawing) {
        j->saved[0] = s.r[0], j->saved[1] = s.r[1];
        j->saved[2] = s.r[2], j->saved[3] = s.r[3];
        j->drawing = 1;
    }

    /* the background, then clear the band to it */
    ros_cpu_enter(&s);
    s.r[0] = g->bg, s.r[3] = 0x80, s.r[4] = 0;
    ros_swi(&s, XColourTrans_SetGCOL);
    vdu(16);                                    /* CLG */
    set_origin(-j->band[0], -j->band[1]);
    service_print(0xFFFFFFFFu);
    return NULL;
}

static void band_end(struct job *j)
{
    if (!j->drawing)
        return;
    service_print(0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = j->saved[0], s.r[1] = j->saved[1];
    s.r[2] = j->saved[2], s.r[3] = j->saved[3];
    ros_swi(&s, XOS_SpriteOp);
    j->drawing = 0;
}

/* A job selected again in the middle of a band: output back into the band,
 * as it was, without clearing it.  OvationPro deselects its job between
 * bands (its hourglass, ArtWorks' vectors) and draws the band after it is
 * selected again; PRM3 has a job's output go to the page while it is
 * selected, so the band is where that drawing belongs. */
static os_error *band_resume(struct job *j)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 60 + 512, s.r[1] = sp_area, s.r[2] = sp_sprite, s.r[3] = 0;
    ros_swi(&s, XOS_SpriteOp);
    if (s.v)
        return err(E_SERVICE, "Output could not be switched to the page");
    j->saved[0] = s.r[0], j->saved[1] = s.r[1];
    j->saved[2] = s.r[2], j->saved[3] = s.r[3];
    j->drawing = 1;
    set_origin(-j->band[0], -j->band[1]);
    service_print(0xFFFFFFFFu);
    return NULL;
}

/* Deselect a job: output back to where it was, and if a band was being
 * drawn, remember to go back into it */
static void job_suspend(struct job *j)
{
    if (j->drawing) {
        band_end(j);
        j->suspended = 1;
    }
}

/* The band's pixels, into the rectangle's image: rows from the top down,
 * as a sprite's run and a PDF image wants, three bytes to the pixel.  A
 * rectangle goes to the page as one image, not one a band, so that a
 * viewer smoothing each image has no seams to show. */
static os_error *band_send(struct job *j, const struct give *g)
{
    uint32_t w = sp_w, h = sp_h;
    if (!j->img) {
        j->img_w = w, j->img_h = (uint32_t)(g->r[3] - g->r[1]);
        if (!(j->img = malloc((size_t)j->img_w * j->img_h * 3)))
            return err(E_NOROOM, "There is not enough memory to print");
    }
    uint32_t image = sp_sprite + ros_ld32(sp_sprite + 32);
    uint32_t top = (uint32_t)(g->r[3] - j->band[3]);    /* the band's top row in the image */
    for (uint32_t y = 0; y < h && top + y < j->img_h; y++) {
        uint8_t *row = j->img + (size_t)(top + y) * j->img_w * 3;
        uint32_t src = image + y * w * 4;               /* w words of &00BBGGRR */
        for (uint32_t x = 0; x < w && x < j->img_w; x++) {
            uint32_t px = ros_ld32(src + x * 4);
            row[x * 3 + 0] = (uint8_t)(px & 0xFF);
            row[x * 3 + 1] = (uint8_t)((px >> 8) & 0xFF);
            row[x * 3 + 2] = (uint8_t)((px >> 16) & 0xFF);
        }
    }
    return NULL;
}

/* The rectangle's image, and the matrix that puts it on the page.
 *
 * A page point is pos + M (p - the rectangle's corner), in OS units, and an
 * OS unit is 1/180 inch = 0.4 point.  The image's unit square covers the
 * rectangle, so its matrix is M scaled by the rectangle's size in points,
 * and its origin is pos. */
static os_error *rect_send(struct job *j, const struct give *g)
{
    if (!j->img)
        return NULL;
    uint32_t w = j->img_w, h = j->img_h;
    double m0 = g->m[0] / 65536.0, m1 = g->m[1] / 65536.0;
    double m2 = g->m[2] / 65536.0, m3 = g->m[3] / 65536.0;
    os_error *er = service_say(j, "image %u %u %g %g %g %g %g %g\n", w, h,
                               0.4 * w * m0, 0.4 * w * m1, 0.4 * h * m2, 0.4 * h * m3,
                               g->pos[0] / 1000.0, g->pos[1] / 1000.0);
    if (!er)
        er = service_write(j, j->img, (size_t)w * h * 3);
    free(j->img);
    j->img = NULL;
    return er;
}

/* ---- the walk over a page's rectangles -------------------------------------- */

/* The next band, into R0 (non-zero if there is one), R1's block and R2. */
static os_error *next_band(struct job *j, struct ros_cpu *s)
{
    for (;;) {
        if (j->cur >= j->ngive) {                       /* the page is done */
            band_end(j);
            os_error *e = service_say(j, "endpage\n");
            j->page_open = 0;
            j->ngive = 0;
            s->r[0] = 0;
            return e;
        }
        const struct give *g = &j->give[j->cur];
        int32_t y0 = j->band_y;
        if (y0 >= g->r[3]) {                            /* this one is done */
            os_error *e = rect_send(j, g);
            if (e)
                return e;
            j->cur++;
            j->band_y = j->cur < j->ngive ? j->give[j->cur].r[1] : 0;
            continue;
        }
        int32_t y1 = y0 + BAND_ROWS;
        if (y1 > g->r[3])
            y1 = g->r[3];
        j->band[0] = g->r[0], j->band[1] = y0;
        j->band[2] = g->r[2], j->band[3] = y1;
        os_error *e = band_begin(j, g);
        if (e)
            return e;
        uint32_t blk = s->r[1];
        ros_st32(blk + 0, (uint32_t)j->band[0]);
        ros_st32(blk + 4, (uint32_t)j->band[1]);
        ros_st32(blk + 8, (uint32_t)j->band[2]);
        ros_st32(blk + 12, (uint32_t)j->band[3]);
        s->r[0] = 1;
        s->r[2] = g->id;
        return NULL;
    }
}

/* ---- jobs ------------------------------------------------------------------- */

static struct job *job_of(uint32_t handle)
{
    for (struct job *j = jobs; j; j = j->next)
        if (j->handle == handle)
            return j;
    return NULL;
}

static void pfonts_lose(void);

static void job_free(struct job *j, int kill_it)
{
    band_end(j);
    pfonts_lose();
    service_stop(j, kill_it);
    for (struct job **p = &jobs; *p; p = &(*p)->next)
        if (*p == j) {
            *p = j->next;
            break;
        }
    if (current == j)
        current = NULL;
    free(j->img);
    free(j);
}

static os_error *job_new(uint32_t handle, uint32_t title, struct job **out)
{
    struct job *j = calloc(1, sizeof *j);
    if (!j)
        return err(E_NOROOM, "There is not enough memory to print");
    j->handle = handle;
    j->in_fd = j->out_fd = -1;
    j->paper_x = paper_x, j->paper_y = paper_y;
    memcpy(j->area, area, sizeof area);
    if (title) {
        unsigned n = 0;
        while (n < sizeof j->title - 1) {
            uint8_t c = ros_ld8(title + n);
            if (c < 32 || c > 126)
                break;
            j->title[n++] = (char)c;
        }
        j->title[n] = 0;
    }
    j->next = jobs, jobs = j;
    *out = j;
    return NULL;
}

/* ---- the SWIs --------------------------------------------------------------- */

static os_error *sw_info(struct ros_cpu *s)
{
    static char *name_block;
    if (!name_block) {
        name_block = ros_rma_alloc(sizeof printer_name);
        if (!name_block)
            return err(E_NOROOM, "There is not enough memory to print");
    }
    /* the chosen printer's own name, unless an application named it */
    const char *dn = printer_named ? NULL : pdriver_dest_name();
    snprintf(name_block, sizeof printer_name, "%s", dn ? dn : printer_name);
    s->r[0] = (PDRIVER_TYPE << 16) | PDRIVER_VERSION;
    s->r[1] = PDRIVER_DPI, s->r[2] = PDRIVER_DPI;
    s->r[3] = FEATURES;
    s->r[4] = ros_addr(name_block);
    s->r[5] = PDRIVER_DPI, s->r[6] = PDRIVER_DPI;
    s->r[7] = printer_number;
    return NULL;
}

static os_error *sw_setinfo(struct ros_cpu *s)
{
    if (s->r[4]) {
        unsigned n = 0;
        while (n < sizeof printer_name - 1) {
            uint8_t c = ros_ld8(s->r[4] + n);
            if (c < 32)
                break;
            printer_name[n++] = (char)c;
        }
        printer_name[n] = 0;
        printer_named = 1;
    }
    printer_number = s->r[7];
    return NULL;
}

static os_error *sw_pagesize(struct ros_cpu *s, int set)
{
    if (set) {
        if (current)
            return err(E_INJOB, "The page size cannot be changed while a job is selected");
        paper_x = s->r[1], paper_y = s->r[2];
        area[0] = s->r[3], area[1] = s->r[4], area[2] = s->r[5], area[3] = s->r[6];
        return NULL;
    }
    const struct job *j = current;
    s->r[1] = j ? j->paper_x : paper_x;
    s->r[2] = j ? j->paper_y : paper_y;
    for (int i = 0; i < 4; i++)
        s->r[3 + i] = j ? j->area[i] : area[i];
    return NULL;
}

static os_error *sw_selectjob(struct ros_cpu *s, int illustration)
{
    uint32_t was = current ? current->handle : 0;
    if (s->r[0] == 0) {
        if (current)
            job_suspend(current);
        current = NULL;
        s->r[0] = was;
        return NULL;
    }
    if (illustration)
        return err(E_NOTHERE, "This printer driver cannot make illustrations");
    struct job *j = job_of(s->r[0]);
    if (!j) {
        os_error *e = job_new(s->r[0], s->r[1], &j);
        if (e)
            return e;
    }
    if (j->cancelled)
        return ros_error(j->err_num ? j->err_num : E_CANCELLED, "%s",
                         j->err_text[0] ? j->err_text : "The print job was cancelled");
    if (current && current != j)
        job_suspend(current);
    if (current != j && j->suspended) {
        j->suspended = 0;
        os_error *e = band_resume(j);
        if (e)
            return e;
    }
    current = j;
    s->r[0] = was;
    return NULL;
}

static os_error *sw_giverectangle(struct ros_cpu *s)
{
    struct job *j = current;
    if (!j)
        return err(E_NOJOB, "No print job is selected");
    if (j->ngive >= MAX_RECTS)
        return err(E_TOOMANY, "Too many rectangles have been given for one page");
    struct give *g = &j->give[j->ngive];
    g->id = s->r[0];
    for (int i = 0; i < 4; i++)
        g->r[i] = (int32_t)ros_ld32(s->r[1] + 4u * (uint32_t)i);
    for (int i = 0; i < 4; i++)
        g->m[i] = (int32_t)ros_ld32(s->r[2] + 4u * (uint32_t)i);
    g->pos[0] = (int32_t)ros_ld32(s->r[3]);
    g->pos[1] = (int32_t)ros_ld32(s->r[3] + 4);
    g->bg = s->r[4];
    if (g->r[2] <= g->r[0] || g->r[3] <= g->r[1])
        return NULL;                            /* an empty rectangle: nothing to draw */
    if (g->r[2] - g->r[0] > MAX_BAND_W)
        return err(E_NOROOM, "The rectangle to print is too wide");
    j->ngive++;
    return NULL;
}

static os_error *sw_drawpage(struct ros_cpu *s)
{
    struct job *j = current;
    if (!j)
        return err(E_NOJOB, "No print job is selected");
    if (j->ngive == 0) {
        s->r[0] = 0;
        return NULL;
    }
    os_error *e = service_start(j);
    if (e)
        return e;
    double wpt = j->paper_x / 1000.0, hpt = j->paper_y / 1000.0;
    if ((e = service_say(j, "page %g %g\n", wpt, hpt)) != NULL)
        return e;
    j->page_open = 1, j->pages++;
    j->cur = 0;
    j->band_y = j->give[0].r[1];
    return next_band(j, s);
}

static os_error *sw_getrectangle(struct ros_cpu *s)
{
    struct job *j = current;
    if (!j)
        return err(E_NOJOB, "No print job is selected");
    if (!j->page_open) {
        s->r[0] = 0;
        return NULL;
    }
    os_error *e = band_send(j, &j->give[j->cur]);
    if (e)
        return e;
    j->band_y = j->band[3];
    return next_band(j, s);
}

static os_error *sw_endjob(struct ros_cpu *s, int abort)
{
    struct job *j = job_of(s->r[0]);
    if (!j)
        return err(E_BADJOB, "That is not a print job");
    os_error *e = NULL;
    if (abort) {
        service_stop(j, 1);
    } else {
        band_end(j);
        if (j->page_open) {
            service_say(j, "endpage\n");
            j->page_open = 0;
        }
        e = service_finish(j);
    }
    job_free(j, 0);
    return e;
}

static os_error *sw_cancel(struct ros_cpu *s, int with_error)
{
    struct job *j = job_of(s->r[0]);
    if (!j)
        return err(E_BADJOB, "That is not a print job");
    j->cancelled = 1;
    if (with_error && s->r[1]) {
        j->err_num = ros_ld32(s->r[1]);
        unsigned n = 0;
        while (n < sizeof j->err_text - 1) {
            uint8_t c = ros_ld8(s->r[1] + 4 + n);
            if (c < 32)
                break;
            j->err_text[n++] = (char)c;
        }
        j->err_text[n] = 0;
    }
    band_end(j);
    service_stop(j, 1);
    if (current == j)
        current = NULL;
    return NULL;
}

/* ---- the thunks ------------------------------------------------------------- */

static void done(struct ros_cpu *s, os_error *e)
{
    if (e)
        ros_swi_fail(s, e);
    else
        s->v = 0;
}

void ros_thunk_PDriver_Info(struct ros_cpu *s) { done(s, sw_info(s)); }
void ros_thunk_PDriver_SetInfo(struct ros_cpu *s) { done(s, sw_setinfo(s)); }

void ros_thunk_PDriver_CheckFeatures(struct ros_cpu *s)
{
    if ((FEATURES & s->r[0]) != (s->r[1] & s->r[0]))
        done(s, err(E_FEATURES, "The printer cannot do what this document needs"));
}

void ros_thunk_PDriver_PageSize(struct ros_cpu *s) { done(s, sw_pagesize(s, 0)); }
void ros_thunk_PDriver_SetPageSize(struct ros_cpu *s) { done(s, sw_pagesize(s, 1)); }
void ros_thunk_PDriver_SelectJob(struct ros_cpu *s) { done(s, sw_selectjob(s, 0)); }
void ros_thunk_PDriver_SelectIllustration(struct ros_cpu *s) { done(s, sw_selectjob(s, 1)); }
void ros_thunk_PDriver_CurrentJob(struct ros_cpu *s)
{
    s->r[0] = current ? current->handle : 0;
    done(s, NULL);
}
void ros_thunk_PDriver_EndJob(struct ros_cpu *s) { done(s, sw_endjob(s, 0)); }
void ros_thunk_PDriver_AbortJob(struct ros_cpu *s) { done(s, sw_endjob(s, 1)); }
void ros_thunk_PDriver_CancelJob(struct ros_cpu *s) { done(s, sw_cancel(s, 0)); }
void ros_thunk_PDriver_CancelJobWithError(struct ros_cpu *s) { done(s, sw_cancel(s, 1)); }
void ros_thunk_PDriver_GiveRectangle(struct ros_cpu *s) { done(s, sw_giverectangle(s)); }
void ros_thunk_PDriver_DrawPage(struct ros_cpu *s) { done(s, sw_drawpage(s)); }
void ros_thunk_PDriver_GetRectangle(struct ros_cpu *s) { done(s, sw_getrectangle(s)); }

void ros_thunk_PDriver_Reset(struct ros_cpu *s)
{
    (void)s;
    while (jobs)
        job_free(jobs, 1);
    current = NULL;
    done(s, NULL);
}

void ros_thunk_PDriver_EnumerateJobs(struct ros_cpu *s)
{
    if (s->r[0] == 0) {
        s->r[0] = jobs ? jobs->handle : 0;
        return;
    }
    struct job *j = job_of(s->r[0]);
    s->r[0] = j && j->next ? j->next->handle : 0;
    done(s, NULL);
}

/* Fonts are declared and kept for a vector page, which is not made.  The
 * raster page draws them with everything else. */
void ros_thunk_PDriver_DeclareFont(struct ros_cpu *s) { done(s, NULL); }

void ros_thunk_PDriver_DeclareDriver(struct ros_cpu *s)
{
    done(s, err(E_NOTHERE, "This box has one printer driver, which cannot be replaced"));
}

void ros_thunk_PDriver_RemoveDriver(struct ros_cpu *s)
{
    done(s, err(E_NOTHERE, "This box has one printer driver, which cannot be removed"));
}

void ros_thunk_PDriver_SelectDriver(struct ros_cpu *s)
{
    uint32_t was = driver_selected ? PDRIVER_TYPE : 0xFFFFFFFFu;
    if ((int32_t)s->r[0] == -2) {
        s->r[0] = was;
        done(s, NULL);
        return;
    }
    if ((int32_t)s->r[0] == -1)
        driver_selected = 0;
    else if (s->r[0] == PDRIVER_TYPE)
        driver_selected = 1;
    else {
        done(s, err(E_NOTHERE, "There is no printer driver of that type"));
        return;
    }
    s->r[0] = was;
    done(s, NULL);
}

void ros_thunk_PDriver_EnumerateDrivers(struct ros_cpu *s)
{
    if (s->r[0] == 0) {
        s->r[0] = 1;                    /* a handle: the one driver */
        s->r[1] = PDRIVER_TYPE;
    } else {
        s->r[0] = 0;
    }
    done(s, NULL);
}

/* MiscOp 0-2 are the printer-independent font calls: nothing is downloaded,
 * so adding a font does nothing and the list is empty. */
void ros_thunk_PDriver_MiscOp(struct ros_cpu *s)
{
    switch (s->r[0]) {
    case 0: case 1:
        done(s, NULL);
        return;
    case 2:
        s->r[3] = 0;                    /* no more font names */
        if (s->r[1] == 0)
            s->r[2] = 0;
        done(s, NULL);
        return;
    default:
        done(s, err(E_NOTHERE, "That printer driver operation is not supported"));
    }
}

void ros_thunk_PDriver_MiscOpForDriver(struct ros_cpu *s)
{
    if (s->r[8] != PDRIVER_TYPE) {
        done(s, err(E_NOTHERE, "There is no printer driver of that type"));
        return;
    }
    ros_thunk_PDriver_MiscOp(s);
}

void ros_thunk_PDriver_SetDriver(struct ros_cpu *s)
{
    done(s, err(E_NOTHERE, "That printer driver operation is not supported"));
}

void ros_thunk_PDriver_SetPrinter(struct ros_cpu *s)
{
    done(s, err(E_NOTHERE, "That printer driver operation is not supported"));
}

void ros_thunk_PDriver_ScreenDump(struct ros_cpu *s)
{
    done(s, err(E_NOTHERE, "This printer driver cannot dump the screen"));
}

void ros_thunk_PDriver_InsertIllustration(struct ros_cpu *s)
{
    done(s, err(E_NOTHERE, "This printer driver cannot insert illustrations"));
}

/* ---- text: Font_Paint, handed over by the Font Manager ----------------------
 *
 * While a band is drawn the Font Manager passes Font_Paint here, through
 * PDriver_FontSWI with R8 the SWI's offset (FontManager's TryPrinterDriver,
 * switched on by Service_Print).  Font_Paint's coordinates are absolute, not
 * from the graphics origin, and the application's fonts were found at the
 * screen's resolution; so the text is painted again with each font found
 * afresh at the band's 180 dpi and the coordinates moved by the band's
 * corner.  The other font calls need nothing: the colours are the Font
 * Manager's own, set while output is in the band. */

#define MAX_PFONTS 32
#define DEFN_MAX  256

struct pfont {
    uint32_t screen, print;             /* the application's handle, and ours */
    uint32_t xs, ys;                    /* its size, 1/16 point */
    char     name[DEFN_MAX];            /* its full name (Font_ReadDefn 'FULL') */
};
static struct pfont pfonts[MAX_PFONTS];
static unsigned npfonts;

static void lose_font(uint32_t h)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = h;
    ros_swi(&s, XFont_LoseFont);
}

static void pfonts_lose(void)
{
    for (unsigned i = 0; i < npfonts; i++)
        lose_font(pfonts[i].print);
    npfonts = 0;
}

/* The printing handle for an application's font, found at 180 dpi the
 * first time and again if the handle has since been reused. */
static os_error *print_font(uint32_t h, uint32_t *out)
{
    struct ros_cpu s;
    for (unsigned i = 0; i < npfonts; i++)
        if (pfonts[i].print == h) {             /* one of ours already */
            *out = h;
            return NULL;
        }
    char *buf = ros_rma_alloc(DEFN_MAX);
    if (!buf)
        return err(E_NOROOM, "There is not enough memory to print");
    ros_cpu_enter(&s);
    s.r[0] = h, s.r[1] = ros_addr(buf), s.r[3] = 0x4C4C5546u;   /* 'FULL' */
    ros_swi(&s, XFont_ReadDefn);
    if (s.v) {
        ros_rma_free(buf);
        return ros_ptr(s.r[0]);
    }
    uint32_t xs = s.r[2], ys = s.r[3];
    buf[DEFN_MAX - 1] = 0;
    if (xs == 0xFFFFFFFFu) {                    /* a master font: as it is */
        ros_rma_free(buf);
        *out = h;
        return NULL;
    }

    struct pfont *p = NULL;
    for (unsigned i = 0; i < npfonts; i++)
        if (pfonts[i].screen == h) {
            p = &pfonts[i];
            if (p->xs == xs && p->ys == ys && !strcmp(p->name, buf)) {
                ros_rma_free(buf);
                *out = p->print;
                return NULL;
            }
            lose_font(p->print);                /* the handle is another font now */
            break;
        }
    if (!p) {
        if (npfonts == MAX_PFONTS)
            pfonts_lose();
        p = &pfonts[npfonts++];
    }

    ros_cpu_enter(&s);
    s.r[1] = ros_addr(buf), s.r[2] = xs, s.r[3] = ys;
    s.r[4] = PDRIVER_DPI, s.r[5] = PDRIVER_DPI;
    ros_swi(&s, XFont_FindFont);
    if (s.v) {
        npfonts--;                              /* the slot is the last, or reused */
        if (p != &pfonts[npfonts])
            *p = pfonts[npfonts];
        ros_rma_free(buf);
        return ros_ptr(s.r[0]);
    }
    p->screen = h, p->print = s.r[0], p->xs = xs, p->ys = ys;
    strcpy(p->name, buf);
    ros_rma_free(buf);
    *out = p->print;
    return NULL;
}

static uint32_t screen_font(uint32_t h)
{
    for (unsigned i = 0; i < npfonts; i++)
        if (pfonts[i].print == h)
            return pfonts[i].screen;
    return h;
}

/* The bytes a control sequence's arguments take after its code at i (the
 * PRM's Font_Paint escapes); 21's comment runs to the next control code,
 * and 27 and 28's matrices start at the next word. */
static uint32_t escape_args(uint32_t str, uint32_t i, uint8_t c)
{
    switch (c) {
    case 9: case 11: case 18: return 3;
    case 17: case 26:         return 1;
    case 19:                  return 7;
    case 25:                  return 2;
    case 21: {
        uint32_t n = 0;
        while (ros_ld8(str + i + 1 + n) >= 32)
            n++;
        return n + 1;
    }
    case 27: case 28: {
        uint32_t at = str + i + 1;
        return ((4 - (at & 3)) & 3) + (c == 27 ? 16 : 24);
    }
    default:                  return 0;
    }
}

static int is_escape(uint8_t c)
{
    return c == 9 || c == 11 || c == 17 || c == 18 || c == 19 || c == 21 ||
           c == 25 || c == 26 || c == 27 || c == 28;
}

static os_error *font_paint(struct job *j, struct ros_cpu *s)
{
    uint32_t R[8];
    memcpy(R, s->r, sizeof R);
    uint32_t flags = R[2];
    struct ros_cpu c;
    os_error *e;

    /* the string's length, then a copy at the same word offset (for 27 and
     * 28) with each font handle (26) changed to its printing one */
    uint32_t str = R[1], limit = flags & 0x80 ? R[7] : 0xFFFFFFFFu, n = 0;
    while (n < limit) {
        uint8_t ch = ros_ld8(str + n);
        if (ch < 32 && !is_escape(ch))
            break;
        n += ch < 32 ? 1 + escape_args(str, n, ch) : 1;
    }
    if (n > limit)
        n = limit;
    uint8_t *base = ros_rma_alloc(n + 8);
    if (!base)
        return err(E_NOROOM, "There is not enough memory to print");
    uint32_t to = ros_addr(base);
    to += (str - to) & 3;
    for (uint32_t i = 0; i < n; i++)
        ros_st8(to + i, ros_ld8(str + i));
    ros_st8(to + n, 0);
    for (uint32_t i = 0; i < n; ) {
        uint8_t ch = ros_ld8(str + i);
        if (ch >= 32) {
            i++;
            continue;
        }
        if (ch == 26 && i + 1 < n) {
            uint32_t ph;
            if ((e = print_font(ros_ld8(str + i + 1), &ph)) != NULL) {
                ros_rma_free(base);
                return e;
            }
            ros_st8(to + i + 1, (uint8_t)ph);
        }
        i += 1 + escape_args(str, i, ch);
    }

    /* the font to start with: R0 if bit 8, else the current one */
    uint32_t h = flags & 0x100 ? R[0] : 0;
    if (!h) {
        ros_cpu_enter(&c);
        ros_swi(&c, XFont_CurrentFont);
        h = c.r[0];
    }
    uint32_t ph = h;
    if (h && (e = print_font(h, &ph)) != NULL) {
        ros_rma_free(base);
        return e;
    }

    /* the band's corner: the coordinates, and a rubout box with them */
    int32_t dx = j->band[0], dy = j->band[1];
    if (!(flags & 0x10))                        /* millipoints, not OS units */
        dx *= 400, dy *= 400;                   /* an OS unit is 400 millipoints */
    uint8_t *blk = NULL;
    if ((flags & 0x22) == 0x22 && R[5]) {
        if (!(blk = ros_rma_alloc(32))) {
            ros_rma_free(base);
            return err(E_NOROOM, "There is not enough memory to print");
        }
        uint32_t b = ros_addr(blk);
        for (uint32_t i = 0; i < 32; i += 4)
            ros_st32(b + i, ros_ld32(R[5] + i));
        for (uint32_t i = 16; i < 32; i += 8) {
            ros_st32(b + i, (uint32_t)((int32_t)ros_ld32(b + i) - dx));
            ros_st32(b + i + 4, (uint32_t)((int32_t)ros_ld32(b + i + 4) - dy));
        }
        R[5] = b;
    }

    /* this Font_Paint is the Font Manager's; its outlines go through Draw,
     * which adds the graphics origin, so that is 0 while it paints */
    service_print(0);
    set_origin(0, 0);
    ros_cpu_enter(&c);
    c.r[0] = ph, c.r[1] = to, c.r[2] = flags | (ph ? 0x100 : 0);
    c.r[3] = (uint32_t)((int32_t)R[3] - dx), c.r[4] = (uint32_t)((int32_t)R[4] - dy);
    c.r[5] = R[5], c.r[6] = R[6], c.r[7] = n;
    c.r[2] |= 0x80;                             /* the copy's length */
    ros_swi(&c, XFont_Paint);
    e = c.v ? ros_ptr(c.r[0]) : NULL;
    set_origin(-j->band[0], -j->band[1]);

    /* the current font, as the application knows it */
    ros_cpu_enter(&c);
    ros_swi(&c, XFont_CurrentFont);
    if (!c.v && c.r[0]) {
        c.r[0] = screen_font(c.r[0]);
        ros_swi(&c, XFont_SetFont);
    }
    service_print(0xFFFFFFFFu);
    ros_rma_free(base);
    if (blk)
        ros_rma_free(blk);
    return e;
}

/* PDriver_FontSWI: R8 is the Font SWI's offset.  Only Font_Paint is handed
 * over; outside a band it is painted as it stands. */
void ros_thunk_PDriver_FontSWI(struct ros_cpu *s)
{
    if (s->r[8] != 0x06) {
        done(s, NULL);
        return;
    }
    if (current && current->drawing) {
        done(s, font_paint(current, s));
        return;
    }
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, s->r, 8 * sizeof c.r[0]);
    service_print(0);
    ros_swi(&c, XFont_Paint);
    done(s, c.v ? ros_ptr(c.r[0]) : NULL);
}

/* SpriteExtend's JPEG plots are not intercepted while the page is pixels
 * (JPEG_PDriverIntercept's bit 0 is left clear, so SpriteExtend plots into
 * the band itself). */
void ros_thunk_PDriver_JPEGSWI(struct ros_cpu *s)
{
    done(s, err(E_NOTHERE, "The printer driver does not take JPEG calls"));
}

/* ---- the module -------------------------------------------------------------- */

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    signal(SIGPIPE, SIG_IGN);           /* a dead writer is an error, not a death */
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)m, (void)fatal;
    while (jobs)
        job_free(jobs, 1);
    if (sp_block)
        ros_rma_free(sp_block), sp_block = NULL, sp_cap = 0;
    return NULL;
}

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6u, "SWI value out of range for module PDriver");
}

struct ros_module pdriver_module = {
    .title = "PDriver",
    .help = "Printer Driver\t1.00 (04 Oct 2026) ROSGD native, PDF",
    .init = init,
    .final = final,
    .bad_swi = bad_swi,
    .swi_chunk = 0x80140,
    .swi_thunks = ros_swi_thunks_PDriver,
    .swi_names = ros_swi_names_PDriver,
    .swi_prefix = "PDriver",
    .commands = pdriver_commands,
};

__attribute__((constructor)) static void count(void)
{
    pdriver_module.swi_count = ros_swi_count_PDriver;
}
