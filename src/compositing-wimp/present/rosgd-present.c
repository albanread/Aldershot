/* SPDX-License-Identifier: MIT */
/* rosgd-present.c -- the GPU presenter (RISCOSGrandDesign design 37, P1).
 *
 * A Linux program of its own, in the box's present root (Alpine, musl),
 * linked against Mesa's libEGL, libgbm and libGLESv2 and libdrm.  It owns
 * the display: RISC OS, in compositor mode (rosgd.display=compositor),
 * draws into a memfd published as /dev/rosgd-screen, with the screen
 * block in its last 4 KB (include/rosgd/screen.h), and this program reads
 * both and shows them, as QEMU's Metal renderer does on the Mac:
 *
 *   upload    the raw screen bytes, the 16 KB chunks that changed, into
 *             a texture (metal-virtio-gpu.c's change detection)
 *   decode    a GLSL ES 3.00 pass: every depth the block names (32, 16
 *             direct; 8, 4, 2, 1 through the palette), either byte
 *             order, pan offsets
 *   scale     to the panel's own mode, centred: exact pixels (nearest)
 *             when the scale is whole, bilinear until the sharp-bilinear
 *             of metal.m 783-798 is ported to follow
 *   flip      eglSwapBuffers on a GBM surface, a KMS page flip, waiting
 *             for the flip event: real vertical sync, three drawables
 *   pointer   the shape and place from the block, on the cursor plane
 *             (to come, with /init's start-and-fall-back)
 *
 * Built in the Alpine build VM by build-in-vm.sh, against the same Mesa
 * the present root runs (Zink on NVK, on the T1000).
 */

#define _GNU_SOURCE
#include <EGL/egl.h>
#ifndef EGL_BUFFER_AGE_EXT
#define EGL_BUFFER_AGE_EXT 0x313D
#endif
#include <EGL/eglext.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/input.h>
#include <strings.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "screen.h"     /* the box's copy: ../../include/rosgd/screen.h */
#include "box-sprite.h" /* the sprite reader shared with metal.m */
#include "presentwire.h" /* the compositor's socket: ../../include/rosgd */

#define BYTES_W 4096    /* the raw screen texture's width; rows follow */

static void ready(void);
static double now_ms(void);
static GLuint try_shader(GLenum kind, const char *src, const char *what);

static struct {
    int card;                       /* the card node, DRM master */
    drmModeConnector *conn;
    drmModeCrtc *crtc;
    uint32_t conn_id, crtc_id, fb_id;
    drmModeModeInfo mode;
    struct gbm_device *gbm;
    struct gbm_surface *gsurf;
    struct gbm_bo *bo;              /* the BO currently scanned out */
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface esurf;
    GLuint dprog, sprog;            /* the decode and scale passes */
    GLuint raw_tex, pal_tex;
    GLuint dec_tex, dec_fbo;        /* the decoded mode, and its target */
    int dec_w, dec_h;
    /* the choices (Choices.Present on the disc, until block v2's present
     * area): the backdrop and the filter */
    int bmode;                      /* metal.m's: 0 none, 1 acorn, 2 acorn-live,
                                       3 picture, 4 tile */
    char spec[192];                 /* the backdrop, in metal.m's grammar */
    GLuint bg_tex;                  /* a picture's or tile's image */
    int bg_w, bg_h;
    float bg_scale;                 /* image pixels to a RISC OS pixel */
    /* a shader scene (bmode 5): its program, its directives, its frames */
    GLuint scn_prog, scn_tex[2], scn_fbo;
    int scn_w, scn_h, scn_cur, scn_feedback, scn_rows, scn_valid;
    float scn_fps, scn_res;         /* res 0: one texel a RISC OS pixel */
    unsigned scn_frame;
    double scn_t0, scn_at;
    int filt;                       /* 0 nearest, 1 sharp-bilinear, 2 linear */
    int scan;                       /* scanlines at 2x and above */
    time_t choices_mtime;
    int choices_read;
    int kbd[16], nkbd;              /* the keyboards, read beside RISC OS */
    /* surfaces the box asks the presenter to draw (design 37, 3.10) */
    GLuint clk_prog, mtr_prog;
    struct ros_screen_surfaces surf;
    uint32_t surf_seq;
    time_t surf_second;             /* the second the clocks show */
    int ctrl, alt;                  /* held, from those devices */
    /* which 16 KB chunks hold a backdrop hole: a scene animates only when
     * it can be seen (metal.m's pacing) */
    uint8_t *chunk_tag;
    uint8_t *chunk_hot;             /* passes left to look at it every pass */
    size_t chunk_n;
    int tagged;
    unsigned tick;                  /* passes, for the whole screen's look */
    /* the screen memory and its block */
    uint8_t *mem;                   /* mmap'd /dev/rosgd-screen */
    size_t mem_size;
    struct ros_screen_block block;
    uint8_t *shadow;                /* our copy, for change detection */
    uint32_t v2_frame;              /* block v2: the last damage frame taken */
    uint32_t surf_v2_frame;         /* v2: the frame its surfaces list was last taken at */
    struct bkent {                  /* v2: the windows with a backing (Sprint 3), and pages */
        uint32_t backing, frame, stride, window;
        int16_t x, y, w, h;
        uint32_t nrects;
        struct ros_v2_rect rect[256];   /* more: the whole of it (drawn back to front) */
        int16_t sh_dx, sh_dy, sh_blur, sh_strength;   /* its shadow: the theme's */
        uint32_t nshadow;
        struct ros_v2_rect shadow[64];
        uint32_t ndamage;               /* what changed in the backing: 0 none, ~0 all */
        struct ros_v2_rect damage[16];
        int caret;                      /* it has the caret (ROS_SURF_WIN_CARET) */
        int radius;                     /* its corners' radius, screen pixels (data[1]) */
        uint32_t page;                  /* ROS_SURF_PAGE: the compositor's id of the page
                                           drawn here, in the stack's order; 0 a backing */
    } bk[ROS_V2_SURFACES];          /* as many as the list holds (the Wimp's backings) */
    GLuint shadow_prog;
    uint32_t nbk;
    int has_pages;                  /* the list places the pages (Sprint 5): draw_wayland
                                       draws only the popups */
    int wimp_only;                  /* the Wimp publishes surfaces: the framebuffer (layer 1)
                                       is neither uploaded nor drawn (Sprint 5) */
    int fb_stale;                   /* its texture is behind: all of it, when it shows again */
    struct {                        /* the backings mapped: by id */
        uint8_t *mem;
        size_t size;
        ino_t ino;                  /* the memfd's: a new one under the same id is remapped */
        GLuint tex;
        uint32_t frame;             /* the entry's frame last uploaded */
        int w, h;
    } bkmap[ROS_V2_SURFACES + 1];
    unsigned missed;                /* v2: chunks the insurance compare found changed */
    uint32_t caret_id;              /* the caret window's backing, looked at for changes */
    uint8_t *caret_copy;            /* what its texture holds */
    size_t caret_bytes;
    double caret_at;
    unsigned caret_fixed;           /* its rows found changed without damage, this minute */
    unsigned missed_said;           /* of them, the ones said this minute */
    size_t pix_size;                /* the screen's bytes, less the block */
    int tex_rows;
    dev_t scr_dev;                  /* the memfd mapped, by its inode: */
    ino_t scr_ino;                  /* the box makes one per mode */
    int scratch;                    /* the egl probe's screen of our own */
    int frame;
    volatile int run;
    volatile int shot;              /* a readback asked for (SIGUSR1) */
    int snap_left, snap_ms, snap_n, snap_tick;   /* a burst of frames (pc-snaps.sh) */
    double snap_next, snap_t0;
    /* the pointer, on the cursor plane where the plane is big enough for
     * the scaled shape, and drawn as a quad in the frame where it is not
     * (as the Mac's renderer draws it) */
    uint32_t cur_handle, cur_pitch, cur_shape_last;
    void *cur_bits;
    int cur_on, cur_x_last, cur_y_last, cur_size, cur_cap, cur_moves;
    int cur_quad, ptr_dirty, ptr_shown_now;
    int32_t ptr_px, ptr_py;
    GLuint ptr_prog, ptr_tex;
    /* the placing of the mode in the panel, for scaling the pointer */
    float s;
    int vx, vy;
    /* Partial redraw (Sprint 5, performance): what changed this pass, in
     * panel pixels, y down; the last frames' damage, newest first, for a
     * back buffer EGL_EXT_buffer_age says is that old; and the clip the
     * frame is drawn through.  A pointer's move redraws the two squares it
     * leaves and enters, not the 4K panel. */
    int remodeset;                  /* set the mode again at the next pass (Ctrl-Alt-F4) */
    uint32_t surf_rate[ROS_SURFACES];   /* the icon surfaces' asked rates, Hz (the entry's stride) */
    int age_ok;                     /* EGL_EXT_buffer_age is offered */
    struct pbox { int x0, y0, x1, y1; } dmg_now, dmg_ring[4];
    int dmg_full, ring_n;
    int clip_on;
    struct pbox clip;
    struct pbox ptr_box;            /* where the pointer quad was drawn */
    int ptr_box_on;
    /* The desktop's look (Sprint 6): the depth the Wimp's desktop was
     * asked for (block v2's look), the screen itself being 32 bpp.  The
     * frame is drawn into look_tex, then shown through the quantising pass
     * in that many colours; the pointer after, in its own. */
    uint32_t look;
    GLuint look_fbo, look_tex, qprog;
    int look_w, look_h;
} P;

#define PBOX_EMPTY ((struct pbox){ 1 << 30, 1 << 30, -(1 << 30), -(1 << 30) })

static void pbox_add(struct pbox *a, struct pbox b)
{
    if (b.x0 >= b.x1 || b.y0 >= b.y1)
        return;
    if (b.x0 < a->x0) a->x0 = b.x0;
    if (b.y0 < a->y0) a->y0 = b.y0;
    if (b.x1 > a->x1) a->x1 = b.x1;
    if (b.y1 > a->y1) a->y1 = b.y1;
}

/* damage in the mode's pixels (screen x, y down), a pixel's margin for the
 * filter */
static void dmg_screen(int x0, int y0, int x1, int y1)
{
    float s = P.s > 0 ? P.s : 1;
    pbox_add(&P.dmg_now, (struct pbox){ (int)floorf(P.vx + x0 * s) - 2, (int)floorf(P.vy + y0 * s) - 2,
                                        (int)ceilf(P.vx + x1 * s) + 2, (int)ceilf(P.vy + y1 * s) + 2 });
}

static void dmg_all(void)
{
    P.dmg_full = 1;
}

/* glScissor through the frame's clip (GL's coordinates, y up) */
static void scissor(int x, int y, int w, int h)
{
    if (P.clip_on) {
        int H = P.mode.vdisplay;
        int cx0 = P.clip.x0, cx1 = P.clip.x1, cy0 = H - P.clip.y1, cy1 = H - P.clip.y0;
        int x1 = x + w, y1 = y + h;
        if (x < cx0) x = cx0;
        if (y < cy0) y = cy0;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
        w = x1 > x ? x1 - x : 0, h = y1 > y ? y1 - y : 0;
    }
    glScissor(x, y, w, h);
}

/* no scissor of a pass's own: the frame's clip, if there is one */
static void scissor_none(void)
{
    if (P.clip_on) {
        glEnable(GL_SCISSOR_TEST);
        scissor(0, 0, P.mode.hdisplay, P.mode.vdisplay);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "rosgd-present: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    /* and to /run/present.log: at boot, stderr is the serial port,
     * which nobody can read from here -- the log can be, over SSH */
    FILE *log = fopen("/run/present.log", "a");
    if (log) {
        va_start(ap, fmt);
        fprintf(log, "rosgd-present: ");
        vfprintf(log, fmt, ap);
        va_end(ap);
        fputc('\n', log);
        fclose(log);
    }
}

static void die(const char *what)
{
    fprintf(stderr, "rosgd-present: %s: %s\n", what, strerror(errno));
    FILE *log = fopen("/run/present.log", "a");
    if (log) {
        fprintf(log, "rosgd-present: %s: %s\n", what, strerror(errno));
        fclose(log);
    }
    exit(1);
}

/* ---- the card, the connector, the CRTC, the mode -------------------------
 * The card with a connected display, as platform/display_drm.c chooses;
 * rosgd.card=N on the kernel's command line names another.  The mode is
 * the connector's preferred one: the panel's own. */

/* wperun hides the cards from the programs it runs (an empty /dev/dri:
 * the browser must not find the GPU, design 23 D3), but the presenter
 * is the GPU's user.  Root inside the chroot can make the nodes itself,
 * from the numbers sysfs gives. */
#include <sys/sysmacros.h>
static void drm_nodes(void)
{
    mkdir("/dev/dri", 0755);
    /* wperun's hiding mount is nodev, and nodes on it cannot be opened
     * (design 23's hiding is thorough); this stack of our own is not */
    mount("tmpfs", "/dev/dri", "tmpfs", MS_NOSUID, "mode=0755");
    /* the cards, and the render nodes too: Vulkan enumerates the render
     * nodes, and Zink on NVK finds no card without them (llvmpipe
     * answers instead, and a software GL is not a presenter) */
    for (int i = 0; i < 16; i++) {
        const char *base = i < 8 ? "card" : "renderD";
        int n = i < 8 ? i : 128 + (i - 8);
        char sys[48], node[32];
        snprintf(sys, sizeof sys, "/sys/class/drm/%s%d/dev", base, n);
        snprintf(node, sizeof node, "/dev/dri/%s%d", base, n);
        FILE *f = fopen(sys, "r");
        if (!f)
            continue;
        unsigned maj = 0, min = 0;
        int ok = fscanf(f, "%u:%u", &maj, &min) == 2;
        fclose(f);
        if (ok && mknod(node, S_IFCHR | 0600, makedev(maj, min)) != 0 &&
            errno != EEXIST)
            say("mknod %s: %s", node, strerror(errno));
    }
}

static int open_card(void)
{
    drm_nodes();
    char cmdline[4096] = "";
    int want = 0;
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, cmdline, sizeof cmdline - 1);
        close(fd);
        if (r > 0)
            cmdline[r] = 0;
        char *p = strstr(cmdline, "rosgd.card=");
        if (p)
            want = atoi(p + 11);
    }
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int c = open(path, O_RDWR | O_CLOEXEC);
        if (c < 0)
            continue;
        drmModeRes *res = drmModeGetResources(c);
        if (!res) {
            close(c);
            continue;
        }
        drmModeConnector *conn = NULL, *any = NULL;
        for (int k = 0; k < res->count_connectors && !conn; k++) {
            drmModeConnector *try = drmModeGetConnector(c, res->connectors[k]);
            if (!try || !try->count_modes) {
                if (try)
                    drmModeFreeConnector(try);
                continue;
            }
            if (try->connection == DRM_MODE_CONNECTED)
                conn = try;
            else if (!any)
                any = try;      /* a panel that has dropped its hot-plug
                                 * (asleep, link down) still lists its
                                 * modes: setting one wakes it */
            else
                drmModeFreeConnector(try);
        }
        if (!conn)
            conn = any;
        if (conn && (want == 0 || want == i)) {
            /* the CRTC: the first this connector can use */
            for (int k = 0; k < res->count_encoders && !P.crtc; k++) {
                drmModeEncoder *e = drmModeGetEncoder(c, conn->encoders[k]);
                if (!e)
                    continue;
                for (int j = 0; j < res->count_crtcs; j++)
                    if (e->possible_crtcs & (1u << j)) {
                        P.crtc = drmModeGetCrtc(c, res->crtcs[j]);
                        P.crtc_id = res->crtcs[j];
                        break;
                    }
                drmModeFreeEncoder(e);
            }
            if (!P.crtc)
                die("no CRTC for the connected display");
            P.conn = conn;
            P.conn_id = conn->connector_id;
            drmModeFreeResources(res);
            return c;
        }
        if (conn)
            drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(c);
    }
    return -1;
}

static void pick_mode(void)
{
    drmModeModeInfo *best = NULL;
    for (int i = 0; i < P.conn->count_modes; i++) {
        drmModeModeInfo *m = &P.conn->modes[i];
        if (m->type & DRM_MODE_TYPE_PREFERRED) {
            best = m;
            break;
        }
        if (!best || m->hdisplay * m->vdisplay > best->hdisplay * best->vdisplay)
            best = m;
    }
    if (!best)
        die("the connector lists no modes");
    P.mode = *best;
    say("mode %ux%u at %u Hz", P.mode.hdisplay, P.mode.vdisplay,
        P.mode.vrefresh);
}

/* ---- GBM and EGL --------------------------------------------------------- */

static void egl_setup(void)
{
    P.gbm = gbm_create_device(P.card);
    if (!P.gbm)
        die("gbm_create_device");
    say("gbm device up");
    /* /init's watch ends HERE: the card is the presenter's, GBM answers,
     * and the slow first start-up (Zink's cold init can take a quarter of
     * a minute) still has the display to itself.  Ready means running,
     * not yet drawing (design 37, 3.8) */
    if (!P.scratch)
        ready();

    P.dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, (void *)P.gbm, NULL);
    if (P.dpy == EGL_NO_DISPLAY)
        P.dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void *)P.gbm, NULL);
    if (P.dpy == EGL_NO_DISPLAY)
        die("eglGetPlatformDisplay (GBM)");
    /* a cold boot races the card: Zink's first start-up can take longer
     * than /init's patience, and a failure here may be the card settling.
     * Retry, for up to half a minute */
    for (int i = 0; ; i++) {
        if (eglInitialize(P.dpy, NULL, NULL))
            break;
        if (i >= 15)
            die("eglInitialize (GBM)");
        say("eglInitialize failed, again in 2 s (retry %d)", i);
        sleep(2);
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(P.dpy, cfg_attr, &cfg, 1, &n) || n < 1)
        die("eglChooseConfig");

    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    P.ctx = eglCreateContext(P.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (P.ctx == EGL_NO_CONTEXT)
        die("eglCreateContext (GLES 3)");

    /* the surface's format is the config's own, as kmscube does it: a
     * GBM surface of another format is refused */
    EGLint vis = 0;
    if (!eglGetConfigAttrib(P.dpy, cfg, EGL_NATIVE_VISUAL_ID, &vis) || !vis)
        vis = GBM_FORMAT_XRGB8888;
    P.gsurf = gbm_surface_create(P.gbm, P.mode.hdisplay, P.mode.vdisplay,
                                 (uint32_t)vis,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!P.gsurf)
        die("gbm_surface_create");
    P.esurf = eglCreatePlatformWindowSurface(P.dpy, cfg, (void *)P.gsurf, NULL);
    if (P.esurf == EGL_NO_SURFACE)
        die("eglCreatePlatformWindowSurface");
    if (!eglMakeCurrent(P.dpy, P.esurf, P.esurf, P.ctx))
        die("eglMakeCurrent");
    {
        const char *x = eglQueryString(P.dpy, EGL_EXTENSIONS);
        P.age_ok = x && strstr(x, "EGL_EXT_buffer_age") != NULL;
        say("partial redraw: %s", P.age_ok ? "EGL_EXT_buffer_age" : "no buffer age: every frame whole");
    }

    const char *r = (const char *)glGetString(GL_RENDERER);
    if (r && (strstr((const char *)r, "llvmpipe") || strstr((const char *)r, "softpipe"))) {
        fprintf(stderr, "rosgd-present: %s answered: software GL is not a "
                "presenter (design 37, 3.8)\n", r);
        exit(1);
    }
    say("renderer: %s", r ? (const char *)r : "?");
}

/* ---- the passes ----------------------------------------------------------
 * metal.m's two, in GLSL ES 3.00 (design 37, 3.2):
 *
 *   decode   screen memory -> a texture the mode's size, one texel a mode
 *            pixel: every depth the block names, either byte order at 32,
 *            pan.  A 32 bpp pixel in the backdrop key colour whose
 *            transfer byte says "below" is a hole, transparent: the
 *            backdrop shows through it.  Drawn only when the screen
 *            changed.
 *   scale    the decoded texture -> the panel: placed centred with its
 *            aspect kept, sharp-bilinear (metal.m 783-798) or nearest at
 *            a whole scale, over the backdrop: c + bg (1 - a).  Drawn
 *            for any change, and for the backdrop's animation.
 *
 * All the vertex shaders are the same quad; pos is location 0 in each. */

static const char *vsh =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *dsh =
    "#version 300 es\n"
    /* highp: byte offsets run to millions; a mediump float rounds them to
     * multiples of 2048 and squashes the picture into a band */
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "out vec4 colour;\n"
    "uniform highp sampler2D raw;    /* the screen's bytes, 4096 wide */\n"
    "uniform sampler2D pal;          /* 256 x 1, from 0x00BBGGRR */\n"
    "uniform int pitch, bpp, pixo;   /* pixo: 32 bpp, 1 red in the low byte */\n"
    "uniform ivec2 pan;              /* the visible area's top left */\n"
    "uniform int holes;              /* 1: a backdrop shows through the key */\n"
    "uniform int test;               /* 1: a flat colour, to bisect the pipe */\n"
    "\n"
    "int byteat(int b) {\n"
    "    return int(texelFetch(raw, ivec2(b & 4095, b >> 12), 0).r * 255.0 + 0.5);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    if (test != 0) { colour = vec4(0.2, 0.4, 0.9, 1.0); return; }\n"
    "    /* the target's row r is the mode's row r */\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy) + pan;\n"
    "    int row = p.y * pitch;\n"
    "    if (bpp == 32) {\n"
    "        int o = row + p.x * 4;\n"
    "        int r = byteat(o), g = byteat(o + 1), b = byteat(o + 2);\n"
    "        int tag = byteat(o + 3);\n"
    "        if (pixo == 0) { int t = r; r = b; b = t; }\n"
    "        /* the transfer byte's layer tag, bits 7-6: 10 is below, and a\n"
    "         * below pixel in the key colour #B7C0B4 is a hole (metal.m) */\n"
    "        if (holes != 0 && (tag & 0xC0) == 0x80 &&\n"
    "            r == 183 && g == 192 && b == 180) {\n"
    "            colour = vec4(0.0);\n"
    "            return;\n"
    "        }\n"
    "        colour = vec4(vec3(r, g, b) / 255.0, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    if (bpp == 24) {\n"
    "        int o = row + p.x * 3;\n"
    "        colour = vec4(vec3(byteat(o), byteat(o + 1), byteat(o + 2)) / 255.0, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    if (bpp == 16) {\n"
    "        int o = row + p.x * 2;\n"
    "        int w = byteat(o) | (byteat(o + 1) << 8);      /* RGB565 */\n"
    "        colour = vec4(float((w >> 11) & 31) / 31.0,\n"
    "                      float((w >> 5) & 63) / 63.0,\n"
    "                      float(w & 31) / 31.0, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    /* 8, 4, 2, 1 bpp: a palette index; below a byte the leftmost pixel\n"
    "     * is in the lowest bits, as RISC OS packs them */\n"
    "    int bit = p.x * bpp;\n"
    "    int byte = byteat(row + (bit >> 3));\n"
    "    int idx = (byte >> (bit & 7)) & ((1 << bpp) - 1);\n"
    "    colour = vec4(texelFetch(pal, ivec2(idx, 0), 0).rgb, 1.0);\n"
    "}\n";

static const char *ssh =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "out vec4 colour;\n"
    "uniform sampler2D dec;          /* the decoded mode, linear */\n"
    "uniform vec2 src;               /* the mode's pixels */\n"
    "uniform vec4 rect;              /* the mode on the panel: x, y down, w, h */\n"
    "uniform float panelh;\n"
    "uniform int filt;               /* 0 nearest, 1 sharp-bilinear, 2 linear */\n"
    "uniform int bmode;              /* 0 none, 1 acorn, 2 acorn-live,\n"
    "                                   3 picture, 4 tile */\n"
    "uniform sampler2D bimg;         /* a picture's or tile's image */\n"
    "uniform vec2 isz;               /* its pixels */\n"
    "uniform float iscale;           /* panel px to one of its pixels */\n"
    "uniform highp sampler2D scn;    /* a shader scene's frame (5) */\n"
    "uniform vec2 bs;                /* its visible texels */\n"
    "uniform float tms;              /* the time, ms, on a half-hour loop */\n"
    "uniform float iconbar;          /* the icon bar's height, mode px */\n"
    "uniform int scan;               /* 1: scanlines, at 2x and above */\n"
    "uniform int layer;              /* 1: the backdrop alone, no framebuffer (Sprint 5) */\n"
    "\n"
    /* the Acorn mark as a signed distance: metal.m's acorn_distance */
    "float acorn_distance(vec2 q) {\n"
    "    const float grow = 0.8;\n"
    "    float ax = abs(q.x);\n"
    "    float outside = max(ax - 16.0, abs(q.y) - 17.0);\n"
    "    if (outside > 1.0) return -outside;\n"
    "    float cy = clamp(q.y, -20.0, -7.0);\n"
    "    float ct = sqrt((cy + 20.0) / 13.0);\n"
    "    float cxb = 32.0 * ct - 17.0 * ct * ct;\n"
    "    float ck = (32.0 - 34.0 * ct) / max(26.0 * ct, 1e-3);\n"
    "    float cap = min((cxb - ax) / sqrt(1.0 + ck * ck), -7.0 - q.y);\n"
    "    float ny = clamp(q.y, -7.0, 18.0);\n"
    "    float lo = 0.0, hi = 1.0;\n"
    "    for (int i = 0; i < 18; i++) {\n"
    "        float t = 0.5 * (lo + hi), u = 1.0 - t;\n"
    "        float by = u * u * u * 18.0 + 3.0 * u * u * t * 18.0\n"
    "                 + 3.0 * u * t * t * 7.0 - t * t * t * 7.0;\n"
    "        if (by > ny) lo = t; else hi = t;\n"
    "    }\n"
    "    float t = 0.5 * (lo + hi), u = 1.0 - t;\n"
    "    float nxb = 3.0 * u * u * t * 9.0 + 3.0 * u * t * t * 15.0 + t * t * t * 13.0;\n"
    "    float ndx = 3.0 * (u * u * 9.0 + 2.0 * u * t * 6.0 - t * t * 2.0);\n"
    "    float ndy = 3.0 * (2.0 * u * t * -11.0 - t * t * 14.0);\n"
    "    float nk = ndx / min(ndy, -1e-3);\n"
    "    float nut = min(min((nxb - ax) / sqrt(1.0 + nk * nk), 18.0 - q.y), q.y + 7.0);\n"
    "    return min(max(cap, nut) + grow, 17.0 - abs(q.y));\n"
    "}\n"
    "\n"
    /* metal.m's backdrop_scene: the sage ground, the mark centred above
     * the icon bar where *Backdrop -Centre puts the watermark, and for
     * acorn-live two soft lights drifting, a vignette, and a dither */
    "vec3 backdrop_scene(vec2 px, vec2 outPx, vec2 srcPx) {\n"
    "    const vec3 ground = vec3(183.0, 192.0, 180.0) / 255.0;\n"
    "    const vec3 ghost  = vec3(173.0, 183.0, 168.0) / 255.0;\n"
    "    vec2 k = outPx / srcPx;\n"
    "    const float unitPx = 220.0 / 34.0;\n"
    "    vec2 centre = vec2(srcPx.x * 0.5, (srcPx.y - iconbar) * 0.5) * k;\n"
    "    float d = acorn_distance((px - centre) / (unitPx * k.y));\n"
    "    float cover = clamp(d * unitPx * k.y + 0.5, 0.0, 1.0);\n"
    "    vec3 c = mix(ground, ghost, cover);\n"
    "    if (bmode == 2) {\n"
    "        float tau = 6.2831853 * tms / 1800000.0;\n"
    "        vec2 uv = px / outPx;\n"
    "        float aspect = outPx.x / outPx.y;\n"
    "        vec2 l1 = vec2(0.5 + 0.34 * sin(7.0 * tau), 0.42 + 0.22 * sin(5.0 * tau + 1.3));\n"
    "        vec2 l2 = vec2(0.5 + 0.34 * sin(11.0 * tau + 2.1), 0.58 + 0.22 * cos(9.0 * tau));\n"
    "        vec2 d1 = (uv - l1) * vec2(aspect, 1.0);\n"
    "        vec2 d2 = (uv - l2) * vec2(aspect, 1.0);\n"
    "        c += 0.075 * exp(-dot(d1, d1) * 3.5) * vec3(1.0, 1.0, 0.96);\n"
    "        c += 0.05 * exp(-dot(d2, d2) * 4.5) * vec3(0.95, 0.72, 0.42);\n"
    "        vec2 v = (uv - 0.5) * vec2(aspect, 1.0);\n"
    "        c *= 1.0 - 0.07 * dot(v, v);\n"
    "        float n = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));\n"
    "        c += (n - 0.5) / 255.0;\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    /* the panel's pixel, y down, then the mode's place on it */\n"
    "    vec2 px = vec2(gl_FragCoord.x, panelh - gl_FragCoord.y) - rect.xy;\n"
    "    if (px.x < 0.0 || px.y < 0.0 || px.x >= rect.z || px.y >= rect.w) {\n"
    "        colour = vec4(0.0, 0.0, 0.0, 1.0);          /* the border */\n"
    "        return;\n"
    "    }\n"
    "    vec2 st = px / rect.zw * src;\n"
    "    if (filt == 1) {\n"
    "        vec2 ratio = max(rect.zw / src, vec2(1.0));\n"
    "        vec2 halfw = 0.5 - 0.5 / ratio;\n"
    "        vec2 i = floor(st);\n"
    "        vec2 f = st - i - 0.5;\n"
    "        st = i + 0.5 + clamp(f, -halfw, halfw);\n"
    "    } else if (filt == 0) {\n"
    "        st = floor(st) + 0.5;\n"
    "    }\n"
    "    vec4 c = layer == 1 ? vec4(0.0) : texture(dec, st / src);\n"
    "    if (scan != 0 && rect.w >= src.y * 1.99 && (int(px.y) & 1) != 0)\n"
    "        c.rgb *= 0.8;\n"
    "    if (bmode == 0) { colour = vec4(c.rgb, 1.0); return; }\n"
    "    vec3 bg;\n"
    "    if (bmode == 5) {\n"
    "        /* a shader's scene, at its own size, stretched over the\n"
    "         * mode's place: bilinear by hand, since a feedback scene's\n"
    "         * float texture need not filter (metal.m) */\n"
    "        vec2 sp = px / rect.zw * bs - 0.5;\n"
    "        vec2 i = floor(sp), f = sp - i;\n"
    "        ivec2 hi = ivec2(bs) - 1;\n"
    "        ivec2 a = clamp(ivec2(i), ivec2(0), hi);\n"
    "        ivec2 b = clamp(ivec2(i) + 1, ivec2(0), hi);\n"
    "        vec3 c00 = texelFetch(scn, a, 0).rgb, c10 = texelFetch(scn, ivec2(b.x, a.y), 0).rgb;\n"
    "        vec3 c01 = texelFetch(scn, ivec2(a.x, b.y), 0).rgb, c11 = texelFetch(scn, b, 0).rgb;\n"
    "        bg = clamp(mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y), 0.0, 1.0);\n"
    "    } else if (bmode == 4) {\n"
    "        /* tile: from the top left, repeated by the sampler, so the\n"
    "         * seams filter like the rest */\n"
    "        bg = texture(bimg, px / (isz * iscale)).rgb;\n"
    "    } else if (bmode == 3) {\n"
    "        /* picture: fills the mode's place, centred, the overhang\n"
    "         * cropped */\n"
    "        float k = max(rect.z / isz.x, rect.w / isz.y);\n"
    "        bg = texture(bimg, (px - 0.5 * rect.zw) / (k * isz) + 0.5).rgb;\n"
    "    } else {\n"
    "        bg = backdrop_scene(px, rect.zw, src);\n"
    "    }\n"
    "    colour = vec4(c.rgb + bg * (1.0 - c.a), 1.0);\n"
    "}\n";

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        say("shader: %.*s", (int)n, log);
        exit(1);
    }
    return s;
}

/* a program of the quad's vertex shader and a fragment shader, pos at 0 */
static GLuint program(const char *vs, const char *fs, const char *what)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof log, &n, log);
        say("%s did not link: %.*s", what, (int)n, log);
        exit(1);
    }
    return p;
}

/* the block's palette into its texture: at the start, at every mode, and
 * whenever RISC OS changes a colour (VDU 19 moves no generation) */
static void palette_upload(void)
{
    uint8_t pal[256 * 4];
    for (int i = 0; i < 256; i++) {
        uint32_t w = P.block.palette[i];
        pal[i * 4 + 0] = w & 0xFF;          /* 0x00BBGGRR: red low */
        pal[i * 4 + 1] = (w >> 8) & 0xFF;
        pal[i * 4 + 2] = (w >> 16) & 0xFF;
        pal[i * 4 + 3] = 0xFF;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, P.pal_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pal);
    glActiveTexture(GL_TEXTURE0);
}

static void tex_params(GLenum filter)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* the decode pass's target, the mode's size: made anew when the size
 * changes */
static void dec_target(void)
{
    int w = (int)P.block.xres, h = (int)P.block.yres;
    if (w == P.dec_w && h == P.dec_h)
        return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, P.dec_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, P.dec_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, P.dec_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        say("the decode target %dx%d is incomplete: %04x", w, h, st);
    P.dec_w = w;
    P.dec_h = h;
}

static void gl_setup(void)
{
    P.dprog = program(vsh, dsh, "the decode pass");
    P.sprog = program(vsh, ssh, "the scale pass");
    say("gl: link ok, error %04x", glGetError());

    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glGenTextures(1, &P.raw_tex);
    glBindTexture(GL_TEXTURE_2D, P.raw_tex);
    tex_params(GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, BYTES_W, P.tex_rows, 0,
                 GL_RED, GL_UNSIGNED_BYTE, P.mem);

    glGenTextures(1, &P.pal_tex);
    glBindTexture(GL_TEXTURE_2D, P.pal_tex);
    tex_params(GL_NEAREST);
    palette_upload();

    glGenTextures(1, &P.dec_tex);
    glBindTexture(GL_TEXTURE_2D, P.dec_tex);
    tex_params(GL_LINEAR);
    glGenFramebuffers(1, &P.dec_fbo);
    dec_target();

    glUseProgram(P.dprog);
    glUniform1i(glGetUniformLocation(P.dprog, "raw"), 0);
    glUniform1i(glGetUniformLocation(P.dprog, "pal"), 1);
    glUseProgram(P.sprog);
    glUniform1i(glGetUniformLocation(P.sprog, "dec"), 0);
    glUniform1i(glGetUniformLocation(P.sprog, "bimg"), 1);
    glUniform1i(glGetUniformLocation(P.sprog, "scn"), 2);
    glGenTextures(2, P.scn_tex);
    glGenFramebuffers(1, &P.scn_fbo);
    glGenTextures(1, &P.bg_tex);
    glBindTexture(GL_TEXTURE_2D, P.bg_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    {
        static const uint8_t sage[4] = { 0xB7, 0xC0, 0xB4, 0xFF };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, sage);
        P.bg_w = P.bg_h = 1;
        P.bg_scale = 1.0f;
    }
    say("gl: passes ready, error %04x", glGetError());

    /* the pointer's own little program, for when it is drawn in the frame
     * (metal.m draws it as a quad too) */
    static const char *pv =
        "#version 300 es\n"
        "in vec2 pos;\n"
        "out vec2 v;\n"
        "void main() {\n"
        "    v = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"  /* image's top row up */
        "    gl_Position = vec4(pos, 0.0, 1.0);\n"
        "}\n";
    static const char *pf =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 v;\n"
        "out vec4 colour;\n"
        "uniform sampler2D shape;\n"
        "void main() { colour = texture(shape, v); }\n";
    P.ptr_prog = program(pv, pf, "the pointer's program");

    /* the look's pass: the frame, as drawn, in the colours of the depth the
     * desktop was asked for.  Nearest colour, as ColourTrans picks it (its
     * weights, green most), no dither: 2 colours by brightness, 4 greys,
     * the Wimp's 16 colours, RISC OS's 256 (each gun two bits and a tint
     * shared by all three), 16 or 256 greys, and 4096, 32K or 64K colours
     * by truncating the guns */
    static const char *qf =
        "#version 300 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "uniform sampler2D scene;\n"
        "uniform int l2, grey, fmt;\n"
        "out vec4 colour;\n"
        "const vec3 W = vec3(2.0, 4.0, 1.0);\n"
        "const vec3 wimp16[16] = vec3[16](\n"
        "    vec3(1.0), vec3(0.866667), vec3(0.733333), vec3(0.6), vec3(0.466667), vec3(0.333333),\n"
        "    vec3(0.2), vec3(0.0), vec3(0.0, 0.266667, 0.6), vec3(0.933333, 0.933333, 0.0),\n"
        "    vec3(0.0, 0.8, 0.0), vec3(0.866667, 0.0, 0.0), vec3(0.933333, 0.933333, 0.733333),\n"
        "    vec3(0.333333, 0.533333, 0.0), vec3(1.0, 0.733333, 0.0), vec3(0.0, 0.733333, 1.0));\n"
        "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "void main() {\n"
        "    vec3 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0).rgb, q = c;\n"
        "    if (l2 >= 4) {\n"
        "        vec3 n = fmt == 1 ? vec3(31.0, 63.0, 31.0) : fmt == 2 ? vec3(15.0) : vec3(31.0);\n"
        "        q = floor(c * n + 0.5) / n;\n"
        "    } else if (l2 == 0) {\n"
        "        q = vec3(lum(c) >= 0.5 ? 1.0 : 0.0);\n"
        "    } else if (l2 == 1 || grey == 1) {\n"
        "        float n = l2 == 1 ? 3.0 : l2 == 2 ? 15.0 : 255.0;\n"
        "        q = vec3(floor(lum(c) * n + 0.5) / n);\n"
        "    } else if (l2 == 2) {\n"
        "        float best = 1e9;\n"
        "        for (int i = 0; i < 16; i++) {\n"
        "            vec3 d = c - wimp16[i];\n"
        "            float e = dot(d * d, W);\n"
        "            if (e < best) { best = e; q = wimp16[i]; }\n"
        "        }\n"
        "    } else {\n"
        "        float best = 1e9;\n"
        "        for (int t = 0; t < 4; t++) {\n"
        "            vec3 hi = clamp(floor((c * 15.0 - float(t)) / 4.0 + 0.5), 0.0, 3.0);\n"
        "            vec3 p = (hi * 4.0 + float(t)) / 15.0, d = c - p;\n"
        "            float e = dot(d * d, W);\n"
        "            if (e < best) { best = e; q = p; }\n"
        "        }\n"
        "    }\n"
        "    colour = vec4(q, 1.0);\n"
        "}\n";
    static const char *qv =
        "#version 300 es\n"
        "in vec2 pos;\n"
        "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    P.qprog = program(qv, qf, "the look's pass");
    glUseProgram(P.qprog);
    glUniform1i(glGetUniformLocation(P.qprog, "scene"), 0);
    glGenFramebuffers(1, &P.look_fbo);
    glGenTextures(1, &P.look_tex);
    glGenTextures(1, &P.ptr_tex);
    glBindTexture(GL_TEXTURE_2D, P.ptr_tex);
    tex_params(GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

/* ---- the flip ------------------------------------------------------------ */

static uint32_t bo_fb(struct gbm_bo *bo)
{
    uint32_t id = (uintptr_t)gbm_bo_get_user_data(bo);
    if (id)
        return id;
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 }, strides[4] =
        { gbm_bo_get_stride(bo) }, offsets[4] = { 0 };
    uint64_t mods[4] = { gbm_bo_get_modifier(bo) };
    uint32_t flags = 0;
    if (mods[0] != DRM_FORMAT_MOD_INVALID &&
        gbm_bo_get_plane_count(bo) == 1) {
        flags = DRM_MODE_FB_MODIFIERS;
        if (drmModeAddFB2WithModifiers(P.card, gbm_bo_get_width(bo),
                                       gbm_bo_get_height(bo),
                                       gbm_bo_get_format(bo),
                                       handles, strides, offsets, mods, &id,
                                       flags) == 0)
            goto done;
    }
    if (drmModeAddFB2(P.card, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                      gbm_bo_get_format(bo), handles, strides, offsets, &id,
                      0) != 0)
        die("drmModeAddFB2");
done:
    gbm_bo_set_user_data(bo, (void *)(uintptr_t)id, NULL);
    return id;
}

static void flip_and_wait(int first)
{
    struct gbm_bo *next = gbm_surface_lock_front_buffer(P.gsurf);
    if (!next)
        die("gbm_surface_lock_front_buffer");
    uint32_t fb = bo_fb(next);
    int e;
    if (first)
        /* the mode is set from the first drawn buffer; a CRTC with no
         * framebuffer is refused */
        e = drmModeSetCrtc(P.card, P.crtc_id, fb, 0, 0, &P.conn_id, 1, &P.mode);
    else
        e = drmModePageFlip(P.card, P.crtc_id, fb,
                            DRM_MODE_PAGE_FLIP_EVENT, NULL);
    if (e != 0)
        die(first ? "drmModeSetCrtc" : "drmModePageFlip");
    if (!first) {
        drmEventContext ev = { .version = 2 };
        /* the event IS the vsync: nothing draws ahead of the display */
        while (P.run) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(P.card, &fds);
            if (select(P.card + 1, &fds, NULL, NULL, NULL) < 0) {
                if (errno == EINTR)
                    continue;
                die("select");
            }
            if (FD_ISSET(P.card, &fds) && drmHandleEvent(P.card, &ev) == 0)
                break;
        }
    }
    if (P.bo)
        gbm_surface_release_buffer(P.gsurf, P.bo);
    P.bo = next;
    P.frame++;
}

/* ---- the screen memory and its block ------------------------------------- */

/* The mode set again, the CRTC off and on, so that the DisplayPort link
 * trains anew: this 4K monitor, woken from deep standby or after a
 * restart, now and then showed nothing though the connector was connected
 * and the CRTC set (15 Oct).  Done once a little after the start, and on
 * Ctrl-Alt-F4 or /run/present-modeset (from the control shell). */
static void remodeset(void)
{
    if (!P.bo)
        return;
    uint32_t fb = bo_fb(P.bo);
    drmModeSetCrtc(P.card, P.crtc_id, 0, 0, 0, NULL, 0, NULL);
    int e = drmModeSetCrtc(P.card, P.crtc_id, fb, 0, 0, &P.conn_id, 1, &P.mode);
    say("mode set again: %s", e ? strerror(errno) : "ok");
    P.cur_on = 0;                               /* the cursor plane, put back */
    P.cur_x_last = P.cur_y_last = INT32_MIN;
    dmg_all();
}

static void open_screen(void)
{
    struct stat st;
    if (getenv("ROSGD_PRESENT_SCRATCH")) {
        /* the egl probe: no box, no block -- a scratch screen of our own,
         * zeros and a made-up block, so the GL path can be tested on a
         * boot where the box drove its own display (no publish) */
        P.mem_size = 1280 * 800 * 4 + ROS_SCREEN_BLOCK;
        P.mem = malloc(P.mem_size);
        memset(P.mem, 0x30, P.mem_size);
        if (getenv("ROSGD_PRESENT_HOLES")) {
            /* all of it a backdrop hole: the key, tagged below, so the
             * probe shows the backdrop alone */
            uint32_t *w = (uint32_t *)P.mem;
            for (size_t i = 0; i < 1280 * 800; i++)
                w[i] = 0x80B4C0B7u;
        }
        memcpy(&P.block, P.mem + P.mem_size - ROS_SCREEN_BLOCK,
               sizeof P.block);
        P.block.magic = 0;
        struct ros_screen_block b = { .magic = ROS_SCREEN_MAGIC,
            .version = 1, .generation = 1,
            .xres = 1280, .yres = 800, .pitch = 5120, .bpp = 32, .pixo = 1 };
        memcpy(P.mem + P.mem_size - ROS_SCREEN_BLOCK, &b, sizeof b);
        memcpy(&P.block, &b, sizeof b);
        P.pix_size = P.mem_size - ROS_SCREEN_BLOCK;
        P.shadow = malloc(P.pix_size);
        memcpy(P.shadow, P.mem, P.pix_size);
        P.tex_rows = (int)((P.pix_size + BYTES_W - 1) / BYTES_W);
        P.scratch = 1;
        return;
    }
    int fd = open("/dev/rosgd-screen", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        die("/dev/rosgd-screen (the box must run in compositor mode)");
    if (fstat(fd, &st) < 0)
        die("fstat /dev/rosgd-screen");
    P.mem_size = st.st_size;
    P.scr_dev = st.st_dev;
    P.scr_ino = st.st_ino;
    /* writable: the presenter's area of the block is ours to write */
    P.mem = mmap(NULL, P.mem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (P.mem == MAP_FAILED)
        die("mmap /dev/rosgd-screen");
    close(fd);
    memcpy(&P.block, P.mem + P.mem_size - ROS_SCREEN_BLOCK, sizeof P.block);
    if (P.block.magic != ROS_SCREEN_MAGIC)
        die("the screen block has no magic: the box has not written it");
    if (P.block.version < ROS_SCREEN_VERSION)
        die("the screen block is older than this program");
    P.pix_size = P.mem_size - ROS_SCREEN_BLOCK;
    P.shadow = malloc(P.pix_size);
    memcpy(P.shadow, P.mem, P.pix_size);
    P.tex_rows = (int)((P.pix_size + BYTES_W - 1) / BYTES_W);
    say("screen: %ux%u at %u bpp, pitch %u, %zu bytes",
        P.block.xres, P.block.yres, P.block.bpp, P.block.pitch, P.pix_size);
}

/* The box makes a new memfd for each mode (display_drm.c's shared_buffer,
 * so that a mode starts clear) and points /dev/rosgd-screen at it, and a
 * boot changes mode twice before the desktop: a presenter that mapped the
 * screen once showed the first one, black, for ever.  So the link is
 * followed, by the memfd it names -- its inode, not the link's text, for
 * the box's fd numbers come round again.  A new memfd whose block is not
 * written yet is looked at again next time.  1 when the screen moved. */
static int screen_follow(void)
{
    if (P.scratch)
        return 0;
    struct stat st;
    if (stat("/dev/rosgd-screen", &st) != 0)
        return 0;                           /* the box between two modes */
    if (st.st_dev == P.scr_dev && st.st_ino == P.scr_ino &&
        (size_t)st.st_size == P.mem_size)
        return 0;
    int fd = open("/dev/rosgd-screen", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) != 0 || st.st_size < 2 * ROS_SCREEN_BLOCK) {
        close(fd);
        return 0;
    }
    uint8_t *m = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return 0;
    struct ros_screen_block b;
    memcpy(&b, m + st.st_size - ROS_SCREEN_BLOCK, sizeof b);
    if (b.magic != ROS_SCREEN_MAGIC || b.version < ROS_SCREEN_VERSION) {
        munmap(m, (size_t)st.st_size);
        return 0;
    }
    munmap(P.mem, P.mem_size);
    P.mem = m;
    P.mem_size = (size_t)st.st_size;
    P.scr_dev = st.st_dev;
    P.scr_ino = st.st_ino;
    P.block = b;
    P.pix_size = P.mem_size - ROS_SCREEN_BLOCK;
    free(P.shadow);
    P.shadow = malloc(P.pix_size);
    if (!P.shadow)
        die("the shadow copy");
    memcpy(P.shadow, P.mem, P.pix_size);
    P.tex_rows = (int)((P.pix_size + BYTES_W - 1) / BYTES_W);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, P.raw_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, BYTES_W, P.tex_rows, 0,
                 GL_RED, GL_UNSIGNED_BYTE, P.mem);
    palette_upload();
    say("screen: followed to a new memfd: %ux%u at %u bpp, pitch %u, %zu bytes, "
        "generation %u", b.xres, b.yres, b.bpp, b.pitch, P.pix_size, b.generation);
    P.chunk_n = 0;                          /* the holes, counted anew */
    P.cur_shape_last = 0;                   /* the pointer's image, anew */
    P.cur_x_last = P.cur_y_last = INT32_MIN;
    P.shot = 1;                             /* and the proof, read back */
    return 1;
}

/* metal-virtio-gpu.c 242-323: 16 KB chunks against a copy; a still screen
 * is looked at one frame in six */
#define CHUNK 16384
/* Whether two chunks differ.  musl's memcmp goes a byte at a time, and
 * the change scan with it cost a tenth of a core on a still 1080p
 * desktop; eight words at a time, which the compiler makes SIMD, is many
 * times quicker.  Chunks are whole 64-byte lines, but a ragged end goes
 * the slow way. */
static int differs(const uint8_t *a, const uint8_t *b, size_t n)
{
    const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b;
    size_t words = n / 64 * 8;
    for (size_t i = 0; i < words; i += 8) {
        uint64_t d = 0;
        for (int k = 0; k < 8; k++)
            d |= x[i + k] ^ y[i + k];
        if (d)
            return 1;
    }
    return memcmp(a + words * 8, b + words * 8, n - words * 8) != 0;
}

/* Whether a chunk holds a backdrop hole: a 32 bpp word in the key colour
 * with the transfer byte's below tag, in either byte order */
static int chunk_has_hole(const uint8_t *p, size_t n)
{
    if (P.block.bpp != 32)
        return 0;
    const uint32_t key = P.block.pixo ? 0x80B4C0B7u : 0x80B7C0B4u;
    const uint32_t *w = (const uint32_t *)p;
    for (size_t i = 0; i < n / 4; i++)
        if ((w[i] & 0xC0FFFFFFu) == key)
            return 1;
    return 0;
}

static void chunk_tag_set(size_t ci, int hole)
{
    if (P.chunk_tag[ci] != hole) {
        P.tagged += hole ? 1 : -1;
        P.chunk_tag[ci] = (uint8_t)hole;
    }
}

static int screen_upload(void)
{
    struct ros_screen_block b;
    memcpy(&b, P.mem + P.mem_size - ROS_SCREEN_BLOCK, sizeof b);
    int changed = 0;
    int dirty = 0;
    if (b.magic != ROS_SCREEN_MAGIC)
        return 0;                           /* mid-write, or retired */
    size_t chunks = (P.pix_size + CHUNK - 1) / CHUNK;
    if (chunks != P.chunk_n) {
        free(P.chunk_tag);
        free(P.chunk_hot);
        P.chunk_tag = calloc(chunks, 1);
        P.chunk_hot = calloc(chunks, 1);
        if (!P.chunk_tag || !P.chunk_hot)
            die("the chunk tags");
        P.chunk_n = chunks;
        P.tagged = 0;
        changed = 1;
    }
    if (b.generation != P.block.generation) {
        P.block = b;
        changed = 1;                        /* a mode change: all of it */
        P.v2_frame = 0;
        palette_upload();
    } else if (b.xoffset != P.block.xoffset || b.yoffset != P.block.yoffset) {
        P.block.xoffset = b.xoffset;        /* a pan, or a bank shown: the */
        P.block.yoffset = b.yoffset;        /* same bytes, read elsewhere */
        dirty = 1;
    }
    if (memcmp(b.palette, P.block.palette, sizeof b.palette) != 0) {
        memcpy(P.block.palette, b.palette, sizeof b.palette);
        palette_upload();
        dirty = 1;
    }
    /* A chunk is four of the texture's rows.  Each run of changed chunks
     * goes up as whole rows, at once: a chunk sent as a single row left
     * three quarters of every change behind, and a screen redrawn after
     * a mode change showed in stripes */
    /* Only the rows shown are compared: the texture always holds what the
     * shadow does, so a hidden bank's changes still differ from the
     * shadow when a pan shows them, and go up then.  The whole of them is
     * looked at one pass in six (metal-virtio-gpu.c's still screen), and
     * a chunk that changed in the last half second every pass, so that
     * typing and moving things keep up: comparing a 1080p desktop's three
     * banks every pass cost half a core, and the icon bar's clock, which
     * changes every second, kept the whole screen at the full rate */
    /* Block v2 (design 37, interim status 5.1): the Wimp says what it drew,
     * as rectangles under a seqlock, and this uploads those rows and
     * compares nothing -- bar one look at the whole screen a second, as
     * insurance against drawing the Wimp did not see (the count of what it
     * finds is in the pacing line, so that each such path gets hooked).
     * Without a v2 area (an older ROM, the Mac) the whole visible screen
     * is compared every pass, as the hot set and the one-in-six were
     * replaced by on 14 October: they made vertical moves tear. */
    ++P.tick;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, P.raw_tex);
    struct ros_screen_v2 *v2 = NULL;
    {
        const struct ros_screen_surfaces *st =
            (const void *)(P.mem + P.mem_size - ROS_SCREEN_BLOCK + ROS_SCREEN_SURF);
        uint32_t off = st->v2_offset;
        if (off && off + sizeof *v2 <= P.pix_size) {
            v2 = (struct ros_screen_v2 *)(P.mem + off);
            if (v2->magic != ROS_V2_MAGIC)
                v2 = NULL;
        }
    }
    if (v2 && P.wimp_only && !changed) {
        /* layer 1 is not shown: the damage frame is taken, for the ack,
         * and nothing uploaded; all of it goes up when it shows again */
        uint32_t f0 = __atomic_load_n(&v2->frame, __ATOMIC_ACQUIRE);
        if (!(f0 & 1u))
            P.v2_frame = f0;
        P.fb_stale = 1;
        return 0;
    }
    if (P.fb_stale) {
        P.fb_stale = 0;
        changed = 1;                            /* shown again: all of it */
    }
    int whole = changed || !v2 || P.tick % 60 == 0;
    if (v2 && !changed) {
        /* the frame, taken while even and unchanged across the copy */
        uint32_t f0 = __atomic_load_n(&v2->frame, __ATOMIC_ACQUIRE);
        if (!(f0 & 1u) && f0 != P.v2_frame) {
            uint32_t n = v2->ndamage, full = v2->full;
            struct ros_v2_rect r[ROS_V2_DAMAGE];
            if (n > ROS_V2_DAMAGE)
                n = ROS_V2_DAMAGE;
            memcpy(r, v2->damage, n * sizeof r[0]);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&v2->frame, __ATOMIC_ACQUIRE) == f0) {
                P.v2_frame = f0;            /* acked once the surfaces have it too (v2_ack) */
                if (full) {
                    whole = 1;
                } else {
                    size_t base = (size_t)P.block.yoffset * P.block.pitch;
                    for (uint32_t i = 0; i < n; i++) {
                        int y0 = r[i].y0 < 0 ? 0 : r[i].y0, y1 = r[i].y1;
                        if (y1 > (int)P.block.yres)
                            y1 = (int)P.block.yres;
                        if (y0 >= y1)
                            continue;
                        /* the rows, as whole texture rows (the texture is
                         * 4096 bytes wide; the screen's rows are not) */
                        size_t a = (base + (size_t)y0 * P.block.pitch) / BYTES_W * BYTES_W;
                        size_t z = base + (size_t)y1 * P.block.pitch;
                        z = (z + BYTES_W - 1) / BYTES_W * BYTES_W;
                        if (z > P.pix_size)
                            z = P.pix_size;
                        if (a >= z)
                            continue;
                        memcpy(P.shadow + a, P.mem + a, z - a);
                        for (size_t o = a / CHUNK * CHUNK; o < z; o += CHUNK) {
                            size_t cn = P.pix_size - o < CHUNK ? P.pix_size - o : CHUNK;
                            chunk_tag_set(o / CHUNK, chunk_has_hole(P.shadow + o, cn));
                        }
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (int)(a / BYTES_W), BYTES_W,
                                        (int)((z - a) / BYTES_W), GL_RED, GL_UNSIGNED_BYTE,
                                        P.mem + a);
                        dirty = 1;
                    }
                }
            }
        }
    }
    if (!whole)
        return dirty;
    int insurance = v2 && !changed;             /* a difference here is a miss */
    size_t lo = (size_t)P.block.yoffset * P.block.pitch;
    size_t hi = lo + (size_t)P.block.yres * P.block.pitch;
    lo = lo / CHUNK * CHUNK;
    if (changed || hi > P.pix_size || lo >= P.pix_size) {
        lo = 0;
        hi = P.pix_size;
    }
    size_t run = 0, end = 0;               /* the run of changed chunks */
    int open_run = 0, found = 0;
    for (size_t o = lo; o <= hi; o += CHUNK) {
        int now = 0;
        size_t ci = o / CHUNK;
        if (o < hi && (whole || P.chunk_hot[ci])) {
            size_t n = P.pix_size - o < CHUNK ? P.pix_size - o : CHUNK;
            if (P.chunk_hot[ci])
                P.chunk_hot[ci]--;
            if (changed || differs(P.mem + o, P.shadow + o, n)) {
                if (insurance) {
                    P.missed++;             /* drawing the Wimp did not report */
                    if (P.missed_said < 3 && P.block.pitch) {
                        P.missed_said++;
                        size_t row0 = (o - (size_t)P.block.yoffset * P.block.pitch) / P.block.pitch;
                        say("v2 miss: chunk at byte %zu, screen rows %zu..%zu", o, row0,
                            row0 + (n + P.block.pitch - 1) / P.block.pitch);
                    }
                }
                P.chunk_hot[ci] = 30;
                memcpy(P.shadow + o, P.mem + o, n);
                chunk_tag_set(o / CHUNK, chunk_has_hole(P.shadow + o, n));
                now = found = 1;
                if (!open_run)
                    run = o;
                open_run = 1;
                end = o + n;
            }
        }
        /* A run ends at the first chunk that did not change, or at the
         * end of what is looked at: hi is a whole number of chunks only by
         * luck (1280 x 800 x 4 is, 1920 x 1080 x 4 is not), and a run
         * that reached the last, ragged chunk was never sent -- in the
         * 1080 mode the whole screen after a mode change, then a black
         * band wherever the Wimp drew last (14 Oct) */
        if (open_run && (!now || o + CHUNK > hi)) {
            /* rows run .. end; the screen is whole 4 KB pages, so the
             * rows are whole, but a ragged end goes up on its own */
            int y0 = (int)(run / BYTES_W);
            int rows = (int)((end - run) / BYTES_W);
            if (rows > 0)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, BYTES_W, rows,
                                GL_RED, GL_UNSIGNED_BYTE, P.mem + run);
            size_t tail = (end - run) % BYTES_W;
            if (tail)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0 + rows, (int)tail, 1,
                                GL_RED, GL_UNSIGNED_BYTE,
                                P.mem + run + (size_t)rows * BYTES_W);
            open_run = 0;
            dirty = 1;
        }
    }
    (void)found;
    return dirty;
}

/* ---- the readback ---------------------------------------------------------
 * What the GPU actually drew, as a PPM at /tmp/rosgd-present-shot.ppm after
 * the first frame: the proof for design 37's 1:1 check, and the eyes of
 * whoever is debugging.  (The Mac's renderer was proved the same way.) */

static void screenshot_to(const char *path)
{
    const int w = P.mode.hdisplay, h = P.mode.vdisplay;
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px)
        return;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum e = glGetError();
    const unsigned char *mid = px + ((size_t)(h / 2) * w + w / 2) * 4;
    say("screenshot: read back %dx%d, error %04x, corner %02x %02x %02x, "
        "centre %02x %02x %02x", w, h, e, px[0], px[1], px[2],
        mid[0], mid[1], mid[2]);
    const char *name = path ? path : P.scratch ? "/tmp/rosgd-present-egl.ppm"
                                                : "/tmp/rosgd-present-shot.ppm";
    FILE *f = fopen(name, "wb");
    if (!f) {
        free(px);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    unsigned char *line = malloc((size_t)w * 3);
    for (int y = h - 1; y >= 0 && line; y--) {   /* GL's rows run bottom up */
        const unsigned char *row = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {       /* RGBA in, RGB out, a row at a time:
                                             * three bytes a call took 160 ms a frame */
            line[x * 3] = row[x * 4];
            line[x * 3 + 1] = row[x * 4 + 1];
            line[x * 3 + 2] = row[x * 4 + 2];
        }
        fwrite(line, 1, (size_t)w * 3, f);
    }
    free(line);
    fclose(f);
    free(px);
    say("screenshot: %s", name);
}

static void screenshot(void)
{
    screenshot_to(NULL);
}

/* A burst of frames (tools/pc-snaps.sh): /run/present-snap holds "N MS";
 * every MS milliseconds a frame is forced and read back to
 * /tmp/snap-NNN.ppm, N times, then /run/present-snap.done says what was
 * taken and when.  The order things appear in as a window opens or moves
 * is then a sequence of pictures, not an impression. */
static void snap_check(double t)
{
    if (!P.snap_left) {
        if (++P.snap_tick % 10)
            return;                             /* the file looked at every ten passes */
        FILE *f = fopen("/run/present-snap", "r");
        if (!f)
            return;
        int n = 0, ms = 0;
        if (fscanf(f, "%d %d", &n, &ms) == 2 && n > 0 && n <= 200 && ms >= 0) {
            P.snap_left = n;
            P.snap_ms = ms;
            P.snap_n = 0;
            P.snap_next = t;
            P.snap_t0 = t;
            say("snap: %d frames, %d ms apart", n, ms);
        }
        fclose(f);
        unlink("/run/present-snap");
    }
}

static int snap_due(double t)
{
    return P.snap_left && t >= P.snap_next;
}

static void snap_take(double t)
{
    char name[64];
    snprintf(name, sizeof name, "/tmp/snap-%03d.ppm", P.snap_n);
    screenshot_to(name);
    FILE *f = fopen("/tmp/snap-times.txt", P.snap_n ? "a" : "w");
    if (f) {
        fprintf(f, "snap-%03d %.1f ms\n", P.snap_n, t - P.snap_t0);
        fclose(f);
    }
    P.snap_n++;
    P.snap_left--;
    P.snap_next += P.snap_ms;
    if (!P.snap_left) {
        f = fopen("/run/present-snap.done", "w");
        if (f) {
            fprintf(f, "%d frames, %d ms apart, over %.0f ms\n", P.snap_n, P.snap_ms, t - P.snap_t0);
            fclose(f);
        }
        say("snap: done, %d frames", P.snap_n);
    }
}

/* ---- the pointer ---------------------------------------------------------
 * From the block, onto the hardware cursor plane.  The plane's image is an
 * ARGB dumb buffer of the &AARRGGBB words RISC OS writes (ptr_image says
 * where in the screen's memory).  On a scaled screen the shape scales too,
 * when the scale is 2 or more (3.2): the words are copied nearest into a
 * bigger buffer, and the hot spot with them, so the pointer keeps its
 * place in the desktop's own pixels.  The position moves by ioctl as soon
 * as the block's moves, not at the next frame (3.2): the pointer stays as
 * quick as it is today. */

static void cursor_buffer(int side)
{
    if (P.cur_handle && side == P.cur_size)
        return;
    if (P.cur_handle) {
        munmap(P.cur_bits, (size_t)P.cur_size * P.cur_pitch);
        struct drm_mode_destroy_dumb d = { .handle = P.cur_handle };
        drmIoctl(P.card, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        P.cur_handle = 0;
        P.cur_on = 0;
    }
    struct drm_mode_create_dumb c = { .width = (uint32_t)side,
                                      .height = (uint32_t)side, .bpp = 32 };
    if (drmIoctl(P.card, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0)
        die("the cursor buffer");
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(P.card, DRM_IOCTL_MODE_MAP_DUMB, &m) != 0)
        die("mapping the cursor buffer");
    void *bits = mmap(NULL, c.size, PROT_WRITE, MAP_SHARED, P.card, m.offset);
    if (bits == MAP_FAILED)
        die("mmap the cursor buffer");
    P.cur_handle = c.handle;
    P.cur_pitch = c.pitch;
    P.cur_bits = bits;
    P.cur_size = side;
    P.cur_x_last = P.cur_y_last = INT32_MIN;
}

static void cursor_init(void)
{
    uint64_t cap = 0;
    int w = drmGetCap(P.card, DRM_CAP_CURSOR_WIDTH, &cap) == 0 && cap ? (int)cap : 64;
    int h = drmGetCap(P.card, DRM_CAP_CURSOR_HEIGHT, &cap) == 0 && cap ? (int)cap : 64;
    P.cur_cap = w < h ? w : h;
    say("cursor plane: up to %dx%d", w, h);
    cursor_buffer(64);
}

static void pointer(void)
{
    struct ros_screen_block b;
    memcpy(&b, P.mem + P.mem_size - ROS_SCREEN_BLOCK, sizeof b);
    if (b.generation != P.block.generation)
        return;                  /* a mode change: the next frame handles it */
    P.ptr_shown_now = b.ptr_shown && b.ptr_image;
    P.ptr_px = b.ptr_x;
    P.ptr_py = b.ptr_y;
    if (!P.ptr_shown_now) {
        if (P.cur_on) {
            drmModeSetCursor(P.card, P.crtc_id, 0, 0, 0);
            P.cur_on = 0;
            P.ptr_dirty = 1;             /* the frame must rub it out */
        }
        return;
    }
    /* the shape, scaled with the desktop when the scale is 2 or more.
     * Turing's plane says it takes 256 x 256 but shows the scaled shape
     * no larger than 64, so on a scaled desktop the pointer is drawn in
     * the frame as a quad -- as the Mac's renderer draws it -- and the
     * plane serves only where 64 screen pixels are already the size */
    int want = P.s >= 2.0f ? (int)(64.0f * P.s + 0.5f) : 64;
    int quad = want > 64;
    if (quad != P.cur_quad) {
        P.cur_quad = quad;
        P.ptr_dirty = 1;
        P.cur_shape_last = 0;             /* whichever way, the image is
                                             uploaded to its new home */
        P.cur_x_last = P.cur_y_last = INT32_MIN;
        if (quad && P.cur_on) {
            drmModeSetCursor(P.card, P.crtc_id, 0, 0, 0);
            P.cur_on = 0;
        }
    }
    if (quad) {
        if (b.ptr_shape != P.cur_shape_last) {
            /* &AARRGGBB in, RGBA bytes out */
            uint8_t rgba[64 * 64 * 4];
            const uint32_t *src = (const uint32_t *)(P.mem + b.ptr_image);
            for (int i = 0; i < 64 * 64; i++) {
                uint32_t w = src[i];
                rgba[i * 4 + 0] = (uint8_t)(w >> 16);
                rgba[i * 4 + 1] = (uint8_t)(w >> 8);
                rgba[i * 4 + 2] = (uint8_t)w;
                rgba[i * 4 + 3] = (uint8_t)(w >> 24);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, P.ptr_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            P.cur_shape_last = b.ptr_shape;
            P.ptr_dirty = 1;
        }
        if (b.ptr_x != P.cur_x_last || b.ptr_y != P.cur_y_last) {
            P.cur_x_last = b.ptr_x;
            P.cur_y_last = b.ptr_y;
            P.ptr_dirty = 1;              /* the next frame moves it */
        }
        return;
    }
    if (!P.cur_on || b.ptr_shape != P.cur_shape_last) {
        int side = want;
        cursor_buffer(side);
        const uint32_t *src = (const uint32_t *)(P.mem + b.ptr_image);
        uint32_t *dst = (uint32_t *)P.cur_bits;
        for (int y = 0; y < side; y++) {
            const uint32_t *srow = src + (size_t)(y * 64 / side) * 64;
            uint32_t *drow = dst + (size_t)y * (P.cur_pitch / 4);
            for (int x = 0; x < side; x++)
                drow[x] = srow[x * 64 / side];
        }
        uint32_t hot_x = (b.ptr_hot & 0xFFFF) * (uint32_t)side / 64;
        uint32_t hot_y = ((b.ptr_hot >> 16) & 0xFFFF) * (uint32_t)side / 64;
        if (drmModeSetCursor2(P.card, P.crtc_id, P.cur_handle,
                              (uint32_t)side, (uint32_t)side,
                              hot_x, hot_y) != 0)
            die("drmModeSetCursor2");
        P.cur_on = 1;
        P.cur_shape_last = b.ptr_shape;
        P.cur_x_last = P.cur_y_last = INT32_MIN;    /* place it now, too */
    }
    if (b.ptr_x != P.cur_x_last || b.ptr_y != P.cur_y_last) {
        /* ptr_x and ptr_y are the image's top left, in the screen's own
         * pixels: scaled and centred, they place the cursor exactly */
        drmModeMoveCursor(P.card, P.crtc_id,
                          (int)(b.ptr_x * P.s) + P.vx,
                          (int)(b.ptr_y * P.s) + P.vy);
        if (P.cur_moves < 12 || (P.cur_moves % 60) == 0)
            say("pointer: %d,%d (hot %u,%u, shape %u, image %u)",
                b.ptr_x, b.ptr_y, b.ptr_hot & 0xFFFF,
                (b.ptr_hot >> 16) & 0xFFFF, b.ptr_shape, b.ptr_image);
        P.cur_moves++;
        P.cur_x_last = b.ptr_x;
        P.cur_y_last = b.ptr_y;
    }
}

/* the ready file: /init waits for this (design 37, 3.8) -- written into
 * this root's /run, which is /wpe/run to the box, after the first good
 * frame.  Block v2's present_state (P4) takes the contract over. */
static void ready(void)
{
    FILE *f = fopen("/run/present-ready", "w");
    if (f) {
        fputs("running\n", f);
        fclose(f);
    }
}

/* the program's own file, watched: when it changes on the disc, this
 * process re-execs into the new one (design 37) -- code moves to the
 * stick and is running seconds later, no reboot, and /init's watch of
 * the ready file never notices.  readlink /proc/self/exe names the copy
 * this process actually is. */
static void self_update(void)
{
    static char path[256];
    static time_t mtime;
    static int armed;
    if (!armed) {
        ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        if (n <= 0)
            return;
        path[n] = 0;
        struct stat st;
        if (stat(path, &st) != 0)
            return;
        mtime = st.st_mtim.tv_sec;           /* the host writes; nanoseconds
                                               * would fire on nothing */
        armed = 1;
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_mtim.tv_sec == mtime)
        return;
    say("re-exec: %s changed", path);
    unlink("/run/present-ready");
    char *argv[] = { path, NULL };
    execv(path, argv);
    say("re-exec failed: %s", strerror(errno));   /* and carry on as is */
}

/* ---- main ---------------------------------------------------------------- */

/* ---- the surfaces -----------------------------------------------------------
 * Icons whose pictures the presenter draws itself (design 37, 3.10): the
 * Wimp lists them in the block's surface table, with the parts of each that
 * can be seen, and the presenter draws them over the desktop, clipped to
 * those parts.  Screen memory under them stays still.  The first is
 * DeskClock's face: modules/deskclock/deskclock.c's face_at, in GLSL, with
 * its five styles, sampled 4 x 4 a pixel as DeskClock samples its sprite --
 * but at the panel's own pixels, so it is sharp at any scale.  Drawn
 * premultiplied, its edge blending into the icon bar. */

static const char *clksh =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "out vec4 colour;\n"
    "uniform vec4 box;       /* the icon on the panel: x, y down, w, h */\n"
    "uniform float panelh;\n"
    "uniform int style;\n"
    "uniform vec3 hms;       /* hours, minutes, seconds */\n"
    /* DeskClock's styles: face hour minute second centre tick rim inner,
     * &00BBGGRR, and whether dots and digits */
    "const uint ST[40] = uint[40](\n"
    "  0x00E0F2F8u, 0x00202020u, 0x00A08040u, 0x002020C0u, 0x00202020u, 0x00202020u, 0x00201A10u, 0x00C0A070u,\n"
    "  0x001F1410u, 0x00F0F0F0u, 0x00E0D0B0u, 0x002A8AFFu, 0x00F0F0F0u, 0x00CFF4C4u, 0x00443C2Eu, 0x00A8C56Fu,\n"
    "  0x000E1014u, 0x00E5FF00u, 0x007F3DFFu, 0x004DE9FFu, 0x00FFFFFFu, 0x00E5FF00u, 0x007F3DFFu, 0x00FFE500u,\n"
    "  0x00EEF6EAu, 0x002B3124u, 0x007B8A7Bu, 0x004D67E8u, 0x002B3124u, 0x002F6B4Fu, 0x002F6B4Fu, 0x00BFDCCBu,\n"
    "  0x00181212u, 0x002020F0u, 0x002020F0u, 0x002020F0u, 0x002020F0u, 0x00504040u, 0x00504040u, 0x00504040u);\n"
    "const int DOTS[5] = int[5](0, 0, 0, 1, 1);\n"
    "const int DIGI[5] = int[5](0, 0, 0, 0, 1);\n"
    "const int SEGS[10] = int[10](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);\n"
    "const float PI = 3.14159265358979;\n"
    "vec3 rgb(uint c) {\n"
    "    return vec3(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u)) / 255.0;\n"
    "}\n"
    "float capsule(vec2 p, vec2 a, vec2 b, float r) {\n"
    "    vec2 pa = p - a, v = b - a;\n"
    "    float t = clamp(dot(pa, v) / dot(v, v), 0.0, 1.0);\n"
    "    return length(pa - v * t) - r;\n"
    "}\n"
    "float disc(vec2 p, vec2 c, float r) { return length(p - c) - r; }\n"
    "float hand(vec2 p, float c, float a, float t0, float t1, float hw) {\n"
    "    float s = sin(a), k = cos(a);\n"
    "    return capsule(p, vec2(c + t0 * s, c - t0 * k), vec2(c + t1 * s, c - t1 * k), hw);\n"
    "}\n"
    "bool digit_at(vec2 p, int d, float x0, float y0, float wd, float ht) {\n"
    "    const float hw = 0.75;\n"
    "    float m = y0 + ht / 2.0, x1 = x0 + wd, y1 = y0 + ht;\n"
    "    vec4 seg[7] = vec4[7](vec4(x0, y0, x1, y0), vec4(x1, y0, x1, m), vec4(x1, m, x1, y1),\n"
    "                          vec4(x0, y1, x1, y1), vec4(x0, m, x0, y1), vec4(x0, y0, x0, m),\n"
    "                          vec4(x0, m, x1, m));\n"
    "    for (int i = 0; i < 7; i++)\n"
    "        if (((SEGS[d] >> i) & 1) != 0 && capsule(p, seg[i].xy, seg[i].zw, hw) < 0.0)\n"
    "            return true;\n"
    "    return false;\n"
    "}\n"
    /* the colour's number (face 0 .. inner 7) at p, in the sprite's 34
     * pixels, or -1 where the face is not: DeskClock's face_at */
    "int face_at(vec2 p) {\n"
    "    const float c = 17.0, R = 17.0 - 0.6;\n"
    "    vec2 cc = vec2(c);\n"
    "    if (disc(p, cc, R) >= 0.0) return -1;\n"
    "    if (disc(p, cc, R - 1.7) >= 0.0) return 6;\n"
    "    float h = hms.x, m = hms.y, s = hms.z;\n"
    "    if (DIGI[style] != 0) {\n"
    "        float wd = 5.0, ht = 9.0, gap = 2.6;\n"
    "        float x0 = c - (4.0 * wd + 3.0 * gap + 1.2) / 2.0, y0 = c - ht / 2.0;\n"
    "        int ih = int(h), im = int(m);\n"
    "        int dg[4] = int[4](ih / 10, ih % 10, im / 10, im % 10);\n"
    "        for (int i = 0; i < 4; i++) {\n"
    "            float dx = x0 + float(i) * (wd + gap) + (i >= 2 ? 1.2 : 0.0);\n"
    "            if (digit_at(p, dg[i], dx, y0, wd, ht)) return 1;\n"
    "        }\n"
    "        float kx = c + 0.1;\n"
    "        if ((int(s) & 1) == 0 && (disc(p, vec2(kx, c - 2.2), 0.8) < 0.0 ||\n"
    "                                  disc(p, vec2(kx, c + 2.2), 0.8) < 0.0)) return 1;\n"
    "        return 0;\n"
    "    }\n"
    "    float ha = (mod(h, 12.0) + m / 60.0 + s / 3600.0) / 12.0 * 2.0 * PI;\n"
    "    float ma = (m + s / 60.0) / 60.0 * 2.0 * PI;\n"
    "    float sa = s / 60.0 * 2.0 * PI;\n"
    "    if (disc(p, cc, 1.0) < 0.0) return 3;\n"
    "    if (disc(p, cc, 1.9) < 0.0) return 4;\n"
    "    if (hand(p, c, sa, -3.8, R - 3.0, 0.42) < 0.0) return 3;\n"
    "    if (hand(p, c, ma, -1.5, R - 3.4, 0.95) < 0.0) return 2;\n"
    "    if (hand(p, c, ha, -1.5, R * 0.55, 1.35) < 0.0) return 1;\n"
    "    for (int i = 0; i < 12; i++) {\n"
    "        float a = float(i) / 12.0 * 2.0 * PI;\n"
    "        bool major = i % 3 == 0;\n"
    "        if (DOTS[style] != 0) {\n"
    "            if (major && disc(p, vec2(c + (R - 4.6) * sin(a), c - (R - 4.6) * cos(a)), 1.3) < 0.0)\n"
    "                return 5;\n"
    "        } else if (hand(p, c, a, R - (major ? 5.6 : 4.4), R - 2.9, major ? 0.8 : 0.45) < 0.0) {\n"
    "            return major ? 6 : 5;\n"
    "        }\n"
    "    }\n"
    "    if (disc(p, cc, R - 2.3) >= 0.0) return 7;\n"
    "    return 0;\n"
    "}\n"
    "void main() {\n"
    "    /* this panel pixel's top left, in the icon's box, y down */\n"
    "    vec2 px = vec2(gl_FragCoord.x, panelh - gl_FragCoord.y) - 0.5 - box.xy;\n"
    "    vec2 k = 34.0 / box.zw;          /* the sprite's pixels to one of ours */\n"
    "    vec3 acc = vec3(0.0);\n"
    "    float n = 0.0;\n"
    "    for (int j = 0; j < 4; j++)\n"
    "        for (int i = 0; i < 4; i++) {\n"
    "            int c = face_at((px + (vec2(i, j) + 0.5) / 4.0) * k);\n"
    "            if (c >= 0) {\n"
    "                acc += rgb(ST[style * 8 + c]);\n"
    "                n += 1.0;\n"
    "            }\n"
    "        }\n"
    "    if (n == 0.0) discard;\n"
    "    float a = n / 16.0;\n"
    "    colour = vec4(acc / n * a, a);   /* premultiplied */\n"
    "}\n";

/* DeskMeter's graph: modules/deskmeter/deskmeter.c's draw_graph, in GLSL.
 * The sprite is 52 x 34 pixels: a frame two deep with its corners cut,
 * a dashed line at half height, and a bar two pixels wide for each of
 * the last 24 seconds, green below half, amber to three quarters, red
 * above.  Pixel art, so drawn nearest, a block for each of its pixels. */
static const char *mtrsh =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "out vec4 colour;\n"
    "uniform vec4 box;\n"
    "uniform float panelh;\n"
    "uniform int style;\n"
    "uniform float hist[24];         /* 0 to 255, oldest first */\n"
    /* face low middle high centre grid frame inner: DeskMeter's &00BBGGRR,
     * as red, green, blue, chosen by tests (no array is indexed: this
     * driver's dynamic index into a constant array gave nonsense here) */
    "vec3 pal(int st, int c) {\n"
    "    if (st == 0) {\n"
    "        if (c == 0) return vec3(0.0, 10.0, 0.0);\n"
    "        if (c == 1) return vec3(32.0, 192.0, 32.0);\n"
    "        if (c == 2) return vec3(224.0, 192.0, 32.0);\n"
    "        if (c == 3) return vec3(224.0, 64.0, 32.0);\n"
    "        if (c == 4) return vec3(32.0, 192.0, 32.0);\n"
    "        if (c == 5) return vec3(32.0, 80.0, 32.0);\n"
    "        if (c == 6) return vec3(68.0, 192.0, 32.0);\n"
    "        if (c == 7) return vec3(32.0, 192.0, 32.0);\n"
    "    }\n"
    "    if (st == 1) {\n"
    "        if (c == 0) return vec3(20.0, 20.0, 30.0);\n"
    "        if (c == 1) return vec3(32.0, 192.0, 96.0);\n"
    "        if (c == 2) return vec3(224.0, 192.0, 32.0);\n"
    "        if (c == 3) return vec3(224.0, 64.0, 64.0);\n"
    "        if (c == 4) return vec3(32.0, 192.0, 96.0);\n"
    "        if (c == 5) return vec3(48.0, 48.0, 48.0);\n"
    "        if (c == 6) return vec3(96.0, 96.0, 96.0);\n"
    "        if (c == 7) return vec3(32.0, 192.0, 96.0);\n"
    "    }\n"
    "    return vec3(255.0, 0.0, 255.0);   /* a style it does not know */\n"
    "}\n"
    "int pixel(int x, int y) {      /* the colour's number, or -1: clear */\n"
    "    const int W = 52, H = 34;\n"
    "    int cx = min(x, W - 1 - x), cy = min(y, H - 1 - y);\n"
    "    if (cx + cy < 2 && cx < 2 && cy < 2) return -1;   /* the cut corners */\n"
    "    if (y < 2 || y >= H - 2 || x < 2 || x >= W - 2) return 6;\n"
    "    int gh = H - 6;\n"
    "    int up = H - 1 - y - 3;      /* rows up from the graph's bottom */\n"
    "    int i = (x - 4) / 2;\n"
    "    if (x >= 4 && x < W - 3 && i < 24) {\n"
    "        float frac = floor(hist[i] * 1000.0 / 255.0);\n"
    "        int h = int(floor((float(gh) * frac + 500.0) / 1000.0));\n"
    "        if (frac > 0.0 && up >= 0 && up < h)\n"
    "            return frac < 500.0 ? 1 : frac < 750.0 ? 2 : 3;\n"
    "    }\n"
    "    bool inside = y >= 3 && y < H - 3 && x >= 3 && x < W - 3;\n"
    "    if (inside && y - 3 == gh / 2 && ((x - 3) & 3) < 2) return 5;\n"
    "    return 0;\n"
    "}\n"
    "void main() {\n"
    "    vec2 px = vec2(gl_FragCoord.x, panelh - gl_FragCoord.y) - box.xy;\n"
    "    ivec2 q = ivec2(floor(px / box.zw * vec2(52.0, 34.0)));\n"
    "    if (q.x < 0 || q.y < 0 || q.x > 51 || q.y > 33) discard;\n"
    "    int c = pixel(q.x, q.y);\n"
    "    if (c < 0) discard;\n"
    "    colour = vec4(pal(style, c) / 255.0, 1.0);\n"
    "}\n";

/* the block's own areas */
static uint8_t *block_at(unsigned off)
{
    return P.mem + P.mem_size - ROS_SCREEN_BLOCK + off;
}

/* the presenter's area: "running", and the beat that says so (screen.h) */
static void present_beat(void)
{
    if (P.scratch)
        return;
    struct ros_screen_present *p = (struct ros_screen_present *)block_at(ROS_SCREEN_PRESENT);
    p->state = 2;
    p->caps = ROS_PRESENT_SURFACES;
    __atomic_store_n(&p->beat, p->beat + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&p->magic, ROS_PRESENT_MAGIC, __ATOMIC_RELEASE);
}

/* The surface table, read whole while the Wimp is not writing it: 1 when
 * it changed, or a clock's second has moved */
/* The surfaces list of block v2 (design 37, interim status 5.2), when the
 * Wimp writes one: its icon entries mapped onto the table's shape, so the
 * clock and the meter draw by the same code from either.  1 when an
 * entry's frame moved. */
/* an entry's extent on the screen: its outline, and its shadow's with the
 * blur and the drop */
static void bk_damage_all(const struct bkent *q)
{
    dmg_screen(q->x, q->y, q->x + q->w, q->y + q->h);
    int m = q->sh_blur * 2 + (q->sh_dy < 0 ? -q->sh_dy : q->sh_dy) + q->sh_dx + 4;
    for (uint32_t k = 0; k < q->nshadow; k++)
        dmg_screen(q->shadow[k].x0 - m, q->shadow[k].y0 - m, q->shadow[k].x1 + m, q->shadow[k].y1 + m);
}

static struct bkent bk_was[ROS_V2_SURFACES];
static uint32_t nbk_was;

/* what changed between the last list and this one, as damage */
static void bk_diff(void)
{
    for (uint32_t i = 0; i < P.nbk; i++) {
        const struct bkent *q = &P.bk[i];
        const struct bkent *o = NULL;
        for (uint32_t j = 0; j < nbk_was && !o; j++)
            if (bk_was[j].page == q->page && bk_was[j].window == q->window &&
                (q->page || bk_was[j].backing == q->backing))
                o = &bk_was[j];
        if (!o) {
            bk_damage_all(q);
            continue;
        }
        if (o->x != q->x || o->y != q->y || o->w != q->w || o->h != q->h || o->nrects != q->nrects ||
            memcmp(o->rect, q->rect, q->nrects * sizeof q->rect[0]) != 0 || o->nshadow != q->nshadow ||
            memcmp(o->shadow, q->shadow, q->nshadow * sizeof q->shadow[0]) != 0 || o->caret != q->caret) {
            bk_damage_all(o);
            bk_damage_all(q);
            continue;
        }
        if (!q->page && o->frame != q->frame) {
            /* its content: the rows the Wimp drew, within the outline */
            if (q->ndamage == 0xFFFFFFFFu) {
                dmg_screen(q->x, q->y, q->x + q->w, q->y + q->h);
            } else {
                for (uint32_t k = 0; k < q->ndamage; k++)
                    dmg_screen(q->x, q->y + q->damage[k].y0, q->x + q->w, q->y + q->damage[k].y1);
            }
        }
    }
    for (uint32_t j = 0; j < nbk_was; j++) {
        int kept = 0;
        for (uint32_t i = 0; i < P.nbk && !kept; i++)
            kept = bk_was[j].page == P.bk[i].page && bk_was[j].window == P.bk[i].window &&
                   (P.bk[i].page || bk_was[j].backing == P.bk[i].backing);
        if (!kept)
            bk_damage_all(&bk_was[j]);
    }
}

/* the pages' extents: a page's buffer moved on */
static void dmg_pages(void)
{
    for (uint32_t i = 0; i < P.nbk; i++)
        if (P.bk[i].page)
            for (uint32_t k = 0; k < P.bk[i].nrects; k++)
                dmg_screen(P.bk[i].rect[k].x0, P.bk[i].rect[k].y0, P.bk[i].rect[k].x1, P.bk[i].rect[k].y1);
}

static int surfaces_v2(void)
{
    const struct ros_screen_surfaces *st =
        (const void *)(P.mem + P.mem_size - ROS_SCREEN_BLOCK + ROS_SCREEN_SURF);
    uint32_t off = st->v2_offset;
    if (!off || off + sizeof(struct ros_screen_v2) > P.pix_size)
        return -1;
    struct ros_screen_v2 *v2 = (struct ros_screen_v2 *)(P.mem + off);
    if (v2->magic != ROS_V2_MAGIC)
        return -1;
    uint32_t look = v2->look;
    if (look != P.look) {
        P.look = look;                          /* the desktop's look: all of it again */
        dmg_all();
        say("look: %s", ROS_LOOK_L2BPP(look) < 0 ? "16 million colours" : "the depth asked for");
    }
    uint32_t f0 = __atomic_load_n(&v2->frame, __ATOMIC_ACQUIRE);
    if (f0 & 1u)
        return 0;                               /* being written: as it was */
    uint32_t n = v2->nsurfaces;
    if (!n) {
        P.surf_v2_frame = f0;                   /* nothing of the frame's to take */
        P.wimp_only = 0;
        return -1;                              /* no list: the table */
    }
    if (n > ROS_V2_SURFACES)
        n = ROS_V2_SURFACES;
    struct ros_v2_surface e[ROS_V2_SURFACES];
    struct ros_v2_rect r[ROS_V2_RECTS];
    struct ros_v2_rect sd[ROS_V2_SDAMAGE];
    memcpy(e, v2->surface, n * sizeof e[0]);
    uint32_t nsd = 0;
    for (uint32_t i = 0; i < n; i++)
        if (e[i].ndamage != 0xFFFFFFFFu && e[i].damage + e[i].ndamage > nsd &&
            e[i].damage + e[i].ndamage <= ROS_V2_SDAMAGE)
            nsd = e[i].damage + e[i].ndamage;
    memcpy(sd, v2->sdamage, nsd * sizeof sd[0]);
    uint32_t nr = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (e[i].rects + e[i].nrects > nr && e[i].rects + e[i].nrects <= ROS_V2_RECTS)
            nr = e[i].rects + e[i].nrects;
        if (e[i].shadow_rects + e[i].shadow_nrects > nr && e[i].shadow_rects + e[i].shadow_nrects <= ROS_V2_RECTS)
            nr = e[i].shadow_rects + e[i].shadow_nrects;
    }
    memcpy(r, v2->rect, nr * sizeof r[0]);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&v2->frame, __ATOMIC_ACQUIRE) != f0)
        return 0;                               /* torn: next pass */
    if (f0 == P.surf_v2_frame)
        return 0;
    P.surf_v2_frame = f0;
    int changed = 0;
    struct ros_screen_surfaces now;
    memset(&now, 0, sizeof now);
    memcpy(bk_was, P.bk, P.nbk * sizeof P.bk[0]);
    nbk_was = P.nbk;
    P.nbk = 0;
    P.has_pages = 0;
    P.wimp_only = 1;                        /* a list of surfaces: the Wimp-only desktop */
    for (uint32_t i = 0; i < n; i++) {
        if (e[i].kind == ROS_SURF_PAGE && e[i].param && P.nbk < ROS_V2_SURFACES) {
            struct bkent *q = &P.bk[P.nbk];
            memset(q, 0, sizeof *q);
            q->page = e[i].param;
            q->window = e[i].window;
            q->x = e[i].x, q->y = e[i].y, q->w = e[i].w, q->h = e[i].h;
            for (uint32_t k = 0; k < e[i].nrects && q->nrects < 256 && e[i].rects + k < nr; k++)
                q->rect[q->nrects++] = r[e[i].rects + k];
            P.nbk++;
            P.has_pages = 1;
            changed = 1;
            continue;
        }
        if (e[i].kind == ROS_SURF_WINDOW && e[i].backing && P.nbk < ROS_V2_SURFACES) {
            P.bk[P.nbk].page = 0;
            P.bk[P.nbk].backing = e[i].backing, P.bk[P.nbk].frame = e[i].frame;
            P.bk[P.nbk].stride = e[i].stride, P.bk[P.nbk].window = e[i].window;
            P.bk[P.nbk].x = e[i].x, P.bk[P.nbk].y = e[i].y, P.bk[P.nbk].w = e[i].w, P.bk[P.nbk].h = e[i].h;
            P.bk[P.nbk].nrects = 0;
            if (e[i].nrects > 256) {
                /* more pieces than kept: all of the window.  The surfaces are
                 * drawn back to front, so those in front still cover it; only
                 * the framebuffer's own windows (a menu) could be hidden, until
                 * it is fewer pieces again.  Kept to 16, a window behind a few
                 * others lost pieces (14 Oct: "only bits of windows") */
                P.bk[P.nbk].rect[0] = (struct ros_v2_rect){ e[i].x, e[i].y, (int16_t)(e[i].x + e[i].w),
                                                           (int16_t)(e[i].y + e[i].h) };
                P.bk[P.nbk].nrects = 1;
            }
            for (uint32_t k = 0; k < e[i].nrects && e[i].nrects <= 256 && e[i].rects + k < nr; k++)
                P.bk[P.nbk].rect[P.bk[P.nbk].nrects++] = r[e[i].rects + k];
            P.bk[P.nbk].caret = (e[i].data[0] & ROS_SURF_WIN_CARET) != 0;
            P.bk[P.nbk].radius = e[i].data[1];
            P.bk[P.nbk].sh_dx = e[i].shadow_dx, P.bk[P.nbk].sh_dy = e[i].shadow_dy;
            P.bk[P.nbk].sh_blur = e[i].shadow_blur, P.bk[P.nbk].sh_strength = e[i].shadow_strength;
            P.bk[P.nbk].nshadow = 0;
            for (uint32_t k = 0; k < e[i].shadow_nrects && P.bk[P.nbk].nshadow < 64 &&
                 e[i].shadow_rects + k < nr; k++)
                P.bk[P.nbk].shadow[P.bk[P.nbk].nshadow++] = r[e[i].shadow_rects + k];
            if (e[i].ndamage == 0xFFFFFFFFu || e[i].ndamage > 16) {
                P.bk[P.nbk].ndamage = 0xFFFFFFFFu;      /* all of it */
            } else {
                P.bk[P.nbk].ndamage = 0;
                for (uint32_t k = 0; k < e[i].ndamage && e[i].damage + k < nsd; k++)
                    P.bk[P.nbk].damage[P.bk[P.nbk].ndamage++] = sd[e[i].damage + k];
            }
            P.nbk++;
            changed = 1;
        }
    }
    for (uint32_t i = 0; i < n && now.n < ROS_SURFACES; i++) {
        if (e[i].kind != ROS_SURF_CLOCK && e[i].kind != ROS_SURF_MONITOR)
            continue;                           /* windows: above */
        P.surf_rate[now.n] = e[i].stride;      /* the rate it asks, Hz */
        struct ros_surface *d = &now.s[now.n++];
        d->kind = e[i].kind, d->param = e[i].param;
        d->x = e[i].x, d->y = e[i].y, d->w = e[i].w, d->h = e[i].h;
        memcpy(d->data, e[i].data, sizeof d->data);
        for (uint32_t k = 0; k < e[i].nrects && d->nrects < ROS_SURF_RECTS; k++) {
            const struct ros_v2_rect *q = &r[e[i].rects + k];
            d->rect[d->nrects][0] = q->x0, d->rect[d->nrects][1] = q->y0;
            d->rect[d->nrects][2] = q->x1, d->rect[d->nrects][3] = q->y1;
            d->nrects++;
        }
    }
    if (now.n != P.surf.n || memcmp(now.s, P.surf.s, sizeof now.s) != 0) {
        for (uint32_t k = 0; k < P.surf.n; k++)
            dmg_screen(P.surf.s[k].x, P.surf.s[k].y, P.surf.s[k].x + P.surf.s[k].w, P.surf.s[k].y + P.surf.s[k].h);
        for (uint32_t k = 0; k < now.n; k++)
            dmg_screen(now.s[k].x, now.s[k].y, now.s[k].x + now.s[k].w, now.s[k].y + now.s[k].h);
        memcpy(P.surf.s, now.s, sizeof now.s);
        P.surf.n = now.n;
        changed = 1;
    }
    bk_diff();
    return changed;
}

/* The ack: the frame whose damage both the screen's list and the surfaces'
 * lists were taken from.  The Wimp drops a backing's damage once the ack
 * reaches the frame it was published in; acked by the screen's read alone,
 * a frame published between the surfaces' read and the screen's had its
 * backing damage dropped unseen -- a caret drawn then stayed stale until
 * something else drew in its rows (15 Oct: "the caret needs redraw more
 * often") */
static void v2_ack(void)
{
    const struct ros_screen_surfaces *st =
        (const void *)(P.mem + P.mem_size - ROS_SCREEN_BLOCK + ROS_SCREEN_SURF);
    uint32_t off = st->v2_offset;
    if (!off || off + sizeof(struct ros_screen_v2) > P.pix_size || !P.v2_frame ||
        P.v2_frame != P.surf_v2_frame)
        return;
    struct ros_screen_v2 *v2 = (struct ros_screen_v2 *)(P.mem + off);
    if (v2->magic == ROS_V2_MAGIC && __atomic_load_n(&v2->ack, __ATOMIC_ACQUIRE) != P.v2_frame)
        __atomic_store_n(&v2->ack, P.v2_frame, __ATOMIC_RELEASE);
}

static int surfaces(void)
{
    int v2 = surfaces_v2();
    if (v2 >= 0) {
        /* the list is the source; the clock's second still moves the frame */
        int changed = v2;
        time_t now = time(NULL);
        for (uint32_t i = 0; i < P.surf.n; i++)
            if (P.surf.s[i].kind == ROS_SURF_CLOCK && P.surf.s[i].nrects && now != P.surf_second) {
                P.surf_second = now;
                changed = 1;
            }
        return changed;
    }
    if (P.scratch && getenv("ROSGD_PRESENT_SURFTEST") && !P.surf.n) {
        /* the probe's own: a clock and a meter, with a made-up history */
        struct ros_surface *c = &P.surf.s[0], *m = &P.surf.s[1];
        memset(&P.surf, 0, sizeof P.surf);
        P.surf.n = 2;
        c->kind = ROS_SURF_CLOCK, c->x = 600, c->y = 300, c->w = c->h = 34;
        c->nrects = 1, c->rect[0][0] = 600, c->rect[0][1] = 300, c->rect[0][2] = 634, c->rect[0][3] = 334;
        m->kind = ROS_SURF_MONITOR, m->x = 650, m->y = 300, m->w = 52, m->h = 34;
        m->nrects = 1, m->rect[0][0] = 650, m->rect[0][1] = 300, m->rect[0][2] = 702, m->rect[0][3] = 334;
        for (int k = 0; k < 24; k++)
            m->data[k] = (uint8_t)(k * 11);
        return 1;
    }
    if (P.scratch)
        return 0;
    const struct ros_screen_surfaces *t =
        (const struct ros_screen_surfaces *)block_at(ROS_SCREEN_SURF);
    int changed = 0;
    uint32_t seq = __atomic_load_n(&t->seq, __ATOMIC_ACQUIRE);
    if (!(seq & 1) && seq != P.surf_seq) {
        struct ros_screen_surfaces copy;
        memcpy(&copy, t, sizeof copy);
        if (__atomic_load_n(&t->seq, __ATOMIC_ACQUIRE) == seq) {
            if (copy.n > ROS_SURFACES)
                copy.n = 0;
            P.surf = copy;
            P.surf_seq = seq;
            changed = 1;
        }
    }
    time_t now = time(NULL);
    for (uint32_t i = 0; i < P.surf.n; i++)
        if (P.surf.s[i].kind == ROS_SURF_CLOCK && P.surf.s[i].nrects && now != P.surf_second) {
            P.surf_second = now;
            changed = 1;
        }
    return changed;
}

/* a program of the quad's vertex shader and fs; 0, logged, if it will
 * not build */
static GLuint safe_program(const char *fs, const char *what)
{
    GLuint f = try_shader(GL_FRAGMENT_SHADER, fs, what);
    if (!f)
        return 0;
    GLuint v = try_shader(GL_VERTEX_SHADER, vsh, "the quad's vertex shader");
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        say("%s does not link: left out", what);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void draw_surfaces(int W, int H, float s)
{
    if (!P.surf.n)
        return;
    /* built once; one that will not build is left out, never fatal (a
     * presenter that exits takes the desktop off the panel) */
    static int tried;
    if (!tried) {
        tried = 1;
        if (!P.clk_prog)
            P.clk_prog = safe_program(clksh, "the clock's program");
        if (!P.mtr_prog)
            P.mtr_prog = safe_program(mtrsh, "the meter's program");
    }
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);         /* one reading: the seconds and their fraction */
    time_t tt = rt.tv_sec;
    struct tm lt;
    localtime_r(&tt, &lt);
    glViewport(0, 0, W, H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    for (uint32_t i = 0; i < P.surf.n; i++) {
        const struct ros_surface *e = &P.surf.s[i];
        if (!e->nrects || e->w <= 0 || e->h <= 0)
            continue;
        GLuint prog;
        if (e->kind == ROS_SURF_CLOCK && P.clk_prog) {
            prog = P.clk_prog;
            glUseProgram(prog);
            glUniform1i(glGetUniformLocation(prog, "style"), (GLint)(e->param % 5));
            /* the seconds with their fraction: at the rate the clock asks,
             * a smooth hand */
            glUniform3f(glGetUniformLocation(prog, "hms"), (float)lt.tm_hour,
                        (float)lt.tm_min, (float)lt.tm_sec + (float)rt.tv_nsec / 1e9f);
        } else if (e->kind == ROS_SURF_MONITOR && P.mtr_prog) {
            prog = P.mtr_prog;
            glUseProgram(prog);
            glUniform1i(glGetUniformLocation(prog, "style"), (GLint)(e->param % 2));
            float h[24];
            for (int k = 0; k < 24; k++)
                h[k] = e->data[k];
            glUniform1fv(glGetUniformLocation(prog, "hist"), 24, h);
        } else {
            continue;                       /* a kind this presenter does not know */
        }
        glUniform4f(glGetUniformLocation(prog, "box"), P.vx + e->x * s,
                    P.vy + e->y * s, e->w * s, e->h * s);
        glUniform1f(glGetUniformLocation(prog, "panelh"), (float)H);
        for (uint32_t r = 0; r < e->nrects && r < ROS_SURF_RECTS; r++) {
            const int16_t *q = e->rect[r];
            int x0 = (int)floorf(P.vx + q[0] * s), x1 = (int)ceilf(P.vx + q[2] * s);
            int y0 = (int)floorf(P.vy + q[1] * s), y1 = (int)ceilf(P.vy + q[3] * s);
            if (x1 <= x0 || y1 <= y0)
                continue;
            scissor(x0, H - y1, x1 - x0, y1 - y0);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    scissor_none();
    glDisable(GL_BLEND);
}

/* ---- the Wayland windows ---------------------------------------------------
 * Design 37, 3.11: the WPE edition's compositor, run with -x beside the
 * presenter, renders nothing; it passes each surface's buffer here over
 * PW_SOCKET (presentwire.h).  A DMA-BUF -- the browser's page, rendered on
 * the GPU -- is imported once as a texture, no copy; shared memory is
 * mapped and uploaded when shown.  Each window is drawn where the Wimp's
 * external windows' table puts it, clipped to the parts of it that are
 * seen; popups wherever they are.  After each flip the compositor is told
 * (frame done to its clients), and a buffer no longer on the panel is given
 * back. */

#define WL_BUFS 64
#define WL_SURFS 32
#define WL_RELEASES 128

static struct {
    int listen_fd, fd;
    char in[sizeof(struct pw_msg)];
    size_t in_len;
    int in_fds[PW_PLANES], in_nfds;
    struct wl_buf {
        uint32_t id, kind, w, h, format, stride, offset;
        EGLImageKHR image;
        GLuint tex;
        uint8_t *shm;           /* shared memory, mapped */
        size_t shm_len;
        int uploaded;           /* shm: this showing is in the texture */
    } buf[WL_BUFS];
    struct wl_surf {
        uint32_t id, window, buffer;
        int32_t x, y;
        uint32_t w, h;
        /* a shared-memory surface's own texture (Sprint 5): its buffers'
         * changes go into it, the commits' damage accumulated until it is
         * drawn -- a texture a buffer was a commit or two behind, the
         * client turning its buffers round */
        GLuint stex;
        int sw, sh;
        int dfull;                      /* all of it, at the next upload */
        int dx0, dy0, dx1, dy1;         /* else this, in the buffer's pixels */
    } surf[WL_SURFS];
    uint32_t releases[WL_RELEASES];     /* to give back after the flip */
    int nrel;
    int dirty;
    GLuint prog;
    uint32_t ext_seq;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
} WL = { .listen_fd = -1, .fd = -1 };

static const char *wlsh =
    "#version 300 es\n"
    "precision highp float;\n"
    "out vec4 colour;\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 box;       /* the surface on the panel: x, y down, w, h */\n"
    "uniform float panelh;\n"
    "uniform int bgra;       /* shared memory: the bytes are B G R A */\n"
    "uniform int opaque;     /* an X format: no alpha */\n"
    "uniform int tagholes;   /* a backing: alpha &80 (the \"below\" tag) is a hole */\n"
    "void main() {\n"
    "    vec2 px = vec2(gl_FragCoord.x, panelh - gl_FragCoord.y) - box.xy;\n"
    "    vec4 c = texture(tex, px / box.zw);\n"
    "    if (bgra != 0) c = c.bgra;\n"
    "    if (opaque != 0) c.a = 1.0;\n"
    "    if (tagholes != 0 && abs(c.a - 128.0 / 255.0) < 0.002) c = vec4(0.0);\n"
    "    colour = c;                 /* Wayland's pixels are premultiplied */\n"
    "}\n";

static void wl_listen(void)
{
    if (P.scratch)
        return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", PW_SOCKET);
    unlink(PW_SOCKET);
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
        say("wayland: %s: %s", PW_SOCKET, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    WL.listen_fd = fd;
    WL.create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    WL.destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    WL.image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    const char *ext = eglQueryString(P.dpy, EGL_EXTENSIONS);
    say("wayland: listening on %s; DMA-BUF import %s", PW_SOCKET,
        ext && strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") ? "with modifiers" :
        ext && strstr(ext, "EGL_EXT_image_dma_buf_import") ? "without modifiers" : "NOT offered");
}

static struct wl_buf *wl_buf(uint32_t id)
{
    for (int i = 0; i < WL_BUFS; i++)
        if (WL.buf[i].id == id)
            return &WL.buf[i];
    return NULL;
}

static struct wl_surf *wl_surf(uint32_t id, int make)
{
    struct wl_surf *free_one = NULL;
    for (int i = 0; i < WL_SURFS; i++) {
        if (WL.surf[i].id == id)
            return &WL.surf[i];
        if (!WL.surf[i].id && !free_one)
            free_one = &WL.surf[i];
    }
    if (make && free_one) {
        memset(free_one, 0, sizeof *free_one);
        free_one->id = id;
        return free_one;
    }
    return NULL;
}

static void wl_buf_free(struct wl_buf *b)
{
    if (b->image && WL.destroy_image)
        WL.destroy_image(P.dpy, b->image);
    if (b->tex)
        glDeleteTextures(1, &b->tex);
    if (b->shm)
        munmap(b->shm, b->shm_len);
    memset(b, 0, sizeof *b);
}

/* a buffer described: imported now (DMA-BUF) or mapped (shared memory) */
static void wl_buf_new(const struct pw_msg *m, int *fds, int nfds)
{
    struct wl_buf *b = wl_buf(0);
    if (!b) {
        say("wayland: more than %d buffers", WL_BUFS);
        return;
    }
    b->id = m->id, b->kind = m->kind, b->w = m->width, b->h = m->height;
    b->format = m->format, b->stride = m->stride[0], b->offset = m->offset[0];
    glGenTextures(1, &b->tex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, b->tex);
    tex_params(GL_LINEAR);
    if (m->kind == PW_DMABUF && WL.create_image && WL.image_target && nfds >= (int)m->nplanes) {
        EGLint at[64];
        int n = 0;
        at[n++] = EGL_WIDTH, at[n++] = (EGLint)m->width;
        at[n++] = EGL_HEIGHT, at[n++] = (EGLint)m->height;
        at[n++] = EGL_LINUX_DRM_FOURCC_EXT, at[n++] = (EGLint)m->format;
        static const EGLint fd_a[4] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
        static const EGLint off_a[4] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                         EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
        static const EGLint pitch_a[4] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                           EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
        static const EGLint lo_a[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
        static const EGLint hi_a[4] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };
        for (uint32_t i = 0; i < m->nplanes && i < 4; i++) {
            at[n++] = fd_a[i], at[n++] = fds[i];
            at[n++] = off_a[i], at[n++] = (EGLint)m->offset[i];
            at[n++] = pitch_a[i], at[n++] = (EGLint)m->stride[i];
            if (m->modifier != DRM_FORMAT_MOD_INVALID) {
                at[n++] = lo_a[i], at[n++] = (EGLint)(m->modifier & 0xFFFFFFFFu);
                at[n++] = hi_a[i], at[n++] = (EGLint)(m->modifier >> 32);
            }
        }
        at[n++] = EGL_NONE;
        b->image = WL.create_image(P.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
        if (b->image == EGL_NO_IMAGE_KHR) {
            say("wayland: buffer %u: DMA-BUF import failed (%ux%u %.4s mod %llx): %04x",
                m->id, m->width, m->height, (const char *)&m->format,
                (unsigned long long)m->modifier, eglGetError());
            b->image = NULL;
        } else {
            WL.image_target(GL_TEXTURE_2D, (GLeglImageOES)b->image);
            GLenum e = glGetError();
            if (e)
                say("wayland: buffer %u: the image as a texture: %04x", m->id, e);
        }
    } else if (m->kind == PW_SHM && nfds >= 1) {
        b->shm_len = (size_t)m->offset[0] + (size_t)m->stride[0] * m->height;
        void *map = mmap(NULL, b->shm_len, PROT_READ, MAP_SHARED, fds[0], 0);
        b->shm = map == MAP_FAILED ? NULL : map;
        if (!b->shm)
            say("wayland: buffer %u: shared memory: %s", m->id, strerror(errno));
    }
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < nfds; i++)
        close(fds[i]);                  /* the image, or the mapping, holds it */
}

static void wl_release(uint32_t buffer)
{
    if (buffer && buffer != 0xFFFFFFFFu && WL.nrel < WL_RELEASES)
        WL.releases[WL.nrel++] = buffer;
}

static void wl_send(const struct pw_msg *m)
{
    if (WL.fd >= 0 && send(WL.fd, m, sizeof *m, MSG_NOSIGNAL) != (ssize_t)sizeof *m) {
        say("wayland: the compositor's socket: %s", strerror(errno));
        close(WL.fd);
        WL.fd = -1;
    }
}

static void wl_message(const struct pw_msg *m, int *fds, int nfds)
{
    switch (m->type) {
    case PW_BUFFER:
        wl_buf_new(m, fds, nfds);
        return;
    case PW_SURFACE: {
        struct wl_surf *s = wl_surf(m->id, 1);
        if (!s) {
            if (m->buffer != 0xFFFFFFFFu)
                wl_release(m->buffer);  /* nowhere to show it */
            break;
        }
        s->window = m->window, s->x = m->x, s->y = m->y, s->w = m->w, s->h = m->h;
        if (m->buffer != 0xFFFFFFFFu) {
            struct wl_buf *b = wl_buf(m->buffer);
            if (b && (b->image || b->shm)) {
                wl_release(s->buffer);  /* given back once the new one is shown */
                s->buffer = m->buffer;
                b->uploaded = 0;
                if (m->reserved != 1) {
                    s->dfull = 1;       /* an older compositor: all of it */
                } else if (m->offset[2] > m->offset[0] && m->offset[3] > m->offset[1]) {
                    int x0 = (int)m->offset[0], y0 = (int)m->offset[1];
                    int x1 = (int)m->offset[2], y1 = (int)m->offset[3];
                    if (s->dx1 <= s->dx0 || s->dy1 <= s->dy0) {
                        s->dx0 = x0, s->dy0 = y0, s->dx1 = x1, s->dy1 = y1;
                    } else {
                        if (x0 < s->dx0) s->dx0 = x0;
                        if (y0 < s->dy0) s->dy0 = y0;
                        if (x1 > s->dx1) s->dx1 = x1;
                        if (y1 > s->dy1) s->dy1 = y1;
                    }
                }
            } else {
                /* one this cannot draw (not imported): the last page stays
                 * shown, and this one goes back.  Taken, the window showed
                 * nothing until the next frame -- what was under the page,
                 * black, as a window moved (14 Oct) */
                wl_release(m->buffer);
            }
        }
        break;
    }
    case PW_GONE: {
        struct wl_surf *s = wl_surf(m->id, 0);
        if (s) {
            wl_release(s->buffer);
            if (s->stex)
                glDeleteTextures(1, &s->stex);
            memset(s, 0, sizeof *s);
        }
        break;
    }
    case PW_FORGET: {
        struct wl_buf *b = wl_buf(m->id);
        if (b)
            wl_buf_free(b);
        break;
    }
    }
    for (int i = 0; i < nfds; i++)
        close(fds[i]);
    WL.dirty = 1;
}

/* the compositor: let in, and read; 1 when something changed */
static int wl_poll(void)
{
    if (WL.listen_fd < 0)
        return 0;
    if (WL.fd < 0) {
        int fd = accept4(WL.listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
            WL.fd = fd;
            WL.in_len = 0, WL.in_nfds = 0;
            say("wayland: the compositor is connected");
        }
    }
    while (WL.fd >= 0) {
        char ctl[CMSG_SPACE(sizeof(int) * PW_PLANES)];
        struct iovec iov = { .iov_base = WL.in + WL.in_len, .iov_len = sizeof WL.in - WL.in_len };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = ctl, .msg_controllen = sizeof ctl };
        ssize_t n = recvmsg(WL.fd, &mh, MSG_CMSG_CLOEXEC);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                say("wayland: the compositor has gone");
                close(WL.fd);
                WL.fd = -1;
                for (int i = 0; i < WL_SURFS; i++)
                    memset(&WL.surf[i], 0, sizeof WL.surf[i]);
                for (int i = 0; i < WL_BUFS; i++)
                    if (WL.buf[i].id)
                        wl_buf_free(&WL.buf[i]);
                WL.nrel = 0;
                WL.dirty = 1;
            }
            break;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < k; i++) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                    if (WL.in_nfds < PW_PLANES)
                        WL.in_fds[WL.in_nfds++] = fd;
                    else
                        close(fd);
                }
            }
        WL.in_len += (size_t)n;
        if (WL.in_len < sizeof WL.in)
            continue;
        struct pw_msg m;
        memcpy(&m, WL.in, sizeof m);
        WL.in_len = 0;
        int nfds = WL.in_nfds;
        WL.in_nfds = 0;
        wl_message(&m, WL.in_fds, nfds);
    }
    /* a window moved, or something passed in front of one */
    const struct ros_screen_ext *x = (const struct ros_screen_ext *)block_at(ROS_SCREEN_EXT);
    uint32_t seq = __atomic_load_n(&x->seq, __ATOMIC_ACQUIRE);
    if (seq != WL.ext_seq) {
        WL.ext_seq = seq;
        WL.dirty = 1;
    }
    int d = WL.dirty;
    WL.dirty = 0;
    return d;
}

/* after a flip: frame done to the compositor's clients, and what is no
 * longer on the panel given back */
static void wl_flipped(void)
{
    if (WL.fd < 0)
        return;
    struct pw_msg m = { .type = PW_FRAME };
    wl_send(&m);
    for (int i = 0; i < WL.nrel; i++) {
        struct pw_msg r = { .type = PW_RELEASE, .id = WL.releases[i] };
        wl_send(&r);
    }
    WL.nrel = 0;
}

/* one surface, its buffer's texture over the desktop, in a rectangle of
 * the screen (RISC OS pixels) */
static void wl_quad(struct wl_buf *b, float sx, float sy, float sw, float sh,
                    int cx0, int cy0, int cx1, int cy1, int H, float s)
{
    int x0 = (int)floorf(P.vx + cx0 * s), x1 = (int)ceilf(P.vx + cx1 * s);
    int y0 = (int)floorf(P.vy + cy0 * s), y1 = (int)ceilf(P.vy + cy1 * s);
    if (x1 <= x0 || y1 <= y0)
        return;
    scissor(x0, H - y1, x1 - x0, y1 - y0);
    glUniform4f(glGetUniformLocation(WL.prog, "box"), P.vx + sx * s, P.vy + sy * s, sw * s, sh * s);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    (void)b;
}

/* The windows' backings (Sprint 3): each one's sprite, a texture, drawn
 * at the window's outline through its visible rectangles with the pages'
 * program -- the bytes are R, G, B, A, the alpha real -- after the icon
 * surfaces and before the pages, which lie inside.  Where nothing was
 * drawn into a backing it is clear, so over the framebuffer it changes
 * nothing until the chrome is drawn there: the proof is the border
 * appearing from the backing. */
/* The caret's window (15 Oct: "there is generally an active window with
 * the caret, that should narrow it down").  A change drawn into a backing
 * whose damage was lost stays off the screen until something else draws
 * in those rows; where the caret is, that is seen at once.  So the caret
 * window's backing is compared with what its texture holds, ten times a
 * second, and rows that differ go up -- counted in the pacing line, as
 * the screen's insurance misses are, so each such path can be found. */
static void caret_copied(uint32_t id, const uint8_t *img, int y0, int y1, int w)
{
    if (id != P.caret_id || !P.caret_copy || (size_t)y1 * w * 4 > P.caret_bytes)
        return;
    memcpy(P.caret_copy + (size_t)y0 * w * 4, img + (size_t)y0 * w * 4, (size_t)(y1 - y0) * w * 4);
}

static int caret_check(double t)
{
    if (t - P.caret_at < 100)
        return 0;
    P.caret_at = t;
    uint32_t id = 0;
    for (uint32_t i = 0; i < P.nbk; i++)
        if (P.bk[i].caret)
            id = P.bk[i].backing;
    if (!id || id > ROS_V2_SURFACES || !P.bkmap[id].mem || !P.bkmap[id].w) {
        P.caret_id = 0;
        return 0;
    }
    const uint8_t *sp = P.bkmap[id].mem + 16;
    uint32_t image;
    memcpy(&image, sp + 32, 4);
    int w = P.bkmap[id].w, h = P.bkmap[id].h;
    size_t bytes = (size_t)w * h * 4;
    if (44 + bytes > P.bkmap[id].size - 16)
        return 0;
    const uint8_t *img = sp + image;
    if (id != P.caret_id || bytes != P.caret_bytes) {
        /* a new caret window: its texture is taken as current */
        free(P.caret_copy);
        P.caret_copy = malloc(bytes);
        P.caret_bytes = P.caret_copy ? bytes : 0;
        P.caret_id = P.caret_copy ? id : 0;
        if (P.caret_copy)
            memcpy(P.caret_copy, img, bytes);
        return 0;
    }
    size_t row = (size_t)w * 4;
    int y0 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        if (differs(img + y * row, P.caret_copy + y * row, row)) {
            if (y0 < 0)
                y0 = y;
            y1 = y + 1;
        }
    if (y0 < 0)
        return 0;
    memcpy(P.caret_copy + y0 * row, img + y0 * row, (size_t)(y1 - y0) * row);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, P.bkmap[id].tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, img + y0 * row);
    P.caret_fixed++;
    for (uint32_t i = 0; i < P.nbk; i++)
        if (!P.bk[i].page && P.bk[i].backing == id)
            dmg_screen(P.bk[i].x, P.bk[i].y + y0, P.bk[i].x + P.bk[i].w, P.bk[i].y + y1);
    return 1;
}

/* A page in the list (Sprint 5): the external window's Wayland surface,
 * drawn where the entry places it and through its rectangles, in the
 * stack's order -- so whatever is in front of the browser is drawn after
 * it in the same frame.  A page with nothing to show yet is black, as
 * draw_wayland's were.  The program is the backings', whose uniforms are
 * put back for them after. */
static int shadow_one(uint32_t i, int H, float s);

/* A shared-memory surface's texture brought up to its buffer: all of it
 * when new, resized or asked for, else the commits' damage only, with
 * glTexSubImage2D (an image editor's brush stroke commits at the pointer's
 * rate: 9 MB a commit whole at 1920 x 1200, Sprint 5) */
static void shm_upload(struct wl_surf *sf, struct wl_buf *b)
{
    if (!sf->stex) {
        glGenTextures(1, &sf->stex);
        glBindTexture(GL_TEXTURE_2D, sf->stex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        sf->dfull = 1;
    }
    glBindTexture(GL_TEXTURE_2D, sf->stex);
    int bw = (int)b->w, bh = (int)b->h;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(b->stride / 4));
    if (sf->dfull || sf->sw != bw || sf->sh != bh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, b->shm + b->offset);
        sf->sw = bw, sf->sh = bh;
    } else if (sf->dx1 > sf->dx0 && sf->dy1 > sf->dy0) {
        int x0 = sf->dx0 < 0 ? 0 : sf->dx0, y0 = sf->dy0 < 0 ? 0 : sf->dy0;
        int x1 = sf->dx1 > bw ? bw : sf->dx1, y1 = sf->dy1 > bh ? bh : sf->dy1;
        if (x1 > x0 && y1 > y0) {
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, x0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, y0);
            glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x1 - x0, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE,
                            b->shm + b->offset);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    sf->dfull = 0;
    sf->dx0 = sf->dy0 = sf->dx1 = sf->dy1 = 0;
    b->uploaded = 1;
}

static void draw_page(const struct bkent *q, int H, float s)
{
    struct wl_surf *sf = NULL;
    for (int i = 0; i < WL_SURFS && !sf; i++)
        if (WL.surf[i].id && WL.surf[i].window == q->page && WL.surf[i].buffer)
            sf = &WL.surf[i];
    struct wl_buf *b = sf ? wl_buf(sf->buffer) : NULL;
    if (!b || (!b->image && !b->shm)) {
        glClearColor(0, 0, 0, 1);
        for (uint32_t k = 0; k < q->nrects; k++) {
            int x0 = (int)floorf(P.vx + q->rect[k].x0 * s), x1 = (int)ceilf(P.vx + q->rect[k].x1 * s);
            int y0 = (int)floorf(P.vy + q->rect[k].y0 * s), y1 = (int)ceilf(P.vy + q->rect[k].y1 * s);
            if (x1 > x0 && y1 > y0) {
                scissor(x0, H - y1, x1 - x0, y1 - y0);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        return;
    }
    if (b->shm)
        shm_upload(sf, b);                      /* binds its texture */
    else
        glBindTexture(GL_TEXTURE_2D, b->tex);
    glUniform1i(glGetUniformLocation(WL.prog, "bgra"), b->shm != NULL);
    glUniform1i(glGetUniformLocation(WL.prog, "opaque"),
                b->format == DRM_FORMAT_XRGB8888 || b->format == DRM_FORMAT_XBGR8888);
    glUniform1i(glGetUniformLocation(WL.prog, "tagholes"), 0);
    float sw = (float)(sf->w ? sf->w : b->w), sh = (float)(sf->h ? sf->h : b->h);
    for (uint32_t k = 0; k < q->nrects; k++)
        wl_quad(b, (float)q->x + sf->x, (float)q->y + sf->y, sw, sh, q->rect[k].x0, q->rect[k].y0,
                q->rect[k].x1, q->rect[k].y1, H, s);
    glUniform1i(glGetUniformLocation(WL.prog, "bgra"), 0);
    glUniform1i(glGetUniformLocation(WL.prog, "opaque"), 0);
    glUniform1i(glGetUniformLocation(WL.prog, "tagholes"), 1);
}

static void draw_backings(int W, int H, float s)
{
    if (P.scratch || !P.nbk)
        return;
    if (!WL.prog) {
        WL.prog = safe_program(wlsh, "the Wayland windows' program");
        if (!WL.prog)
            return;
    }
    glViewport(0, 0, W, H);
    glUseProgram(WL.prog);
    glUniform1f(glGetUniformLocation(WL.prog, "panelh"), (float)H);
    glUniform1i(glGetUniformLocation(WL.prog, "tex"), 3);
    glUniform1i(glGetUniformLocation(WL.prog, "bgra"), 0);
    glUniform1i(glGetUniformLocation(WL.prog, "opaque"), 0);
    glUniform1i(glGetUniformLocation(WL.prog, "tagholes"), 1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE3);
    /* a whole scale is drawn nearest, exact to the pixel, so that a hole's
     * alpha is read as it is and no edge blurs */
    GLint filter = s == floorf(s) ? GL_NEAREST : GL_LINEAR;
    for (uint32_t i = 0; i < P.nbk; i++) {
        if (shadow_one(i, H, s)) {
            glUseProgram(WL.prog);              /* the backings' again */
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        if (P.bk[i].page) {
            draw_page(&P.bk[i], H, s);
            continue;
        }
        uint32_t id = P.bk[i].backing;
        if (id > ROS_V2_SURFACES)
            continue;
        char node[64];
        snprintf(node, sizeof node, "/dev/rosgd-backing-%u", id);
        if (P.bkmap[id].mem && P.bkmap[id].frame != P.bk[i].frame) {
            /* the entry moved on: is it still the same memfd?  The Wimp
             * frees a backing and makes another under the same id when a
             * window changes size (the Filer opens its window, then again
             * at its final size); the old mapping showed nothing (14 Oct) */
            struct stat st;
            if (stat(node, &st) < 0 || st.st_ino != P.bkmap[id].ino || (size_t)st.st_size != P.bkmap[id].size) {
                munmap(P.bkmap[id].mem, P.bkmap[id].size);
                P.bkmap[id].mem = NULL;         /* the texture is kept: made again at the new size */
                P.bkmap[id].w = P.bkmap[id].h = 0;
                say("backing %u: remade by the Wimp, mapped again", id);
            }
        }
        if (!P.bkmap[id].mem) {
            int fd = open(node, O_RDONLY | O_CLOEXEC);
            struct stat st;
            if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 60) {
                if (fd >= 0)
                    close(fd);
                continue;
            }
            /* MAP_POPULATE: its pages read in now, not faulted in one at a
             * time by the upload that follows */
            P.bkmap[id].mem = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
            close(fd);
            if (P.bkmap[id].mem == MAP_FAILED) {
                P.bkmap[id].mem = NULL;
                continue;
            }
            P.bkmap[id].size = (size_t)st.st_size;
            P.bkmap[id].ino = st.st_ino;
            if (!P.bkmap[id].tex)
                glGenTextures(1, &P.bkmap[id].tex);
            P.bkmap[id].frame = 0;
            say("backing %u: mapped, %zu bytes", id, P.bkmap[id].size);
        }
        const uint8_t *sp = P.bkmap[id].mem + 16;
        uint32_t words, rows, image;
        memcpy(&words, sp + 16, 4), memcpy(&rows, sp + 20, 4), memcpy(&image, sp + 32, 4);
        int w = (int)words + 1, h = (int)rows + 1;
        if (44 + (size_t)w * 4 * h > P.bkmap[id].size - 16)
            continue;
        glBindTexture(GL_TEXTURE_2D, P.bkmap[id].tex);
        if (P.bkmap[id].w != w || P.bkmap[id].h != h) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, sp + image);
            P.bkmap[id].frame = P.bk[i].frame;
            P.bkmap[id].w = w, P.bkmap[id].h = h;
            caret_copied(id, sp + image, 0, h, w);
        } else if (P.bkmap[id].frame != P.bk[i].frame) {
            /* 5.4: the rows the Wimp drew since, not the backing */
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            if (P.bk[i].ndamage == 0xFFFFFFFFu) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, sp + image);
                caret_copied(id, sp + image, 0, h, w);
            } else {
                for (uint32_t k = 0; k < P.bk[i].ndamage; k++) {
                    int y0 = P.bk[i].damage[k].y0 < 0 ? 0 : P.bk[i].damage[k].y0;
                    int y1 = P.bk[i].damage[k].y1 > h ? h : P.bk[i].damage[k].y1;
                    if (y1 > y0) {
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE,
                                        sp + image + (size_t)y0 * w * 4);
                        caret_copied(id, sp + image, y0, y1, w);
                    }
                }
            }
            P.bkmap[id].frame = P.bk[i].frame;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        for (uint32_t k = 0; k < P.bk[i].nrects; k++)
            wl_quad(NULL, (float)P.bk[i].x, (float)P.bk[i].y, (float)P.bk[i].w, (float)P.bk[i].h,
                    P.bk[i].rect[k].x0, P.bk[i].rect[k].y0, P.bk[i].rect[k].x1, P.bk[i].rect[k].y1, H, s);
    }
    scissor_none();
    glDisable(GL_BLEND);
}

/* The windows' shadows (design 37, interim status 5.2): a darkening of
 * whatever has been drawn, by the theme's strength falling off with the
 * distance to the window's box dropped by the theme's offset, over the
 * blur's extent; scissored to the shadow rectangles the Wimp gives --
 * the shadow's box less the window itself and every window above -- so it
 * darkens what is beneath and beside the window and nothing over it.
 * Drawn last before the pointer.  Overlapping shadows multiply. */
static const char *shadowsh =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 box;        /* the dropped window box on the panel: x, y down, w, h */\n"
    "uniform float blur;      /* panel pixels */\n"
    "uniform float strength;  /* 0..1 at the edge */\n"
    "uniform float panelh;\n"
    "uniform float radius;    /* the window's corners, panel pixels: a rounded box */\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec2 p = vec2(gl_FragCoord.x, panelh - gl_FragCoord.y);\n"
    "    vec2 h = box.zw * 0.5;\n"
    "    float r = min(radius, min(h.x, h.y));\n"
    "    vec2 q = abs(p - (box.xy + h)) - (h - vec2(r));\n"
    "    float dist = max(length(max(q, vec2(0.0))) - r, 0.0);\n"
    "    float t = clamp(1.0 - dist / max(blur, 1.0), 0.0, 1.0);\n"
    "    float a = strength * t * t * (3.0 - 2.0 * t);\n"
    "    frag = vec4(0.0, 0.0, 0.0, a);\n"
    "}\n";

/* One window's shadow, drawn just before its backing (Sprint 5): what is
 * in front of it is drawn after, so a shadow lies only on what is below
 * its window -- drawn over everything once all were drawn, it darkened
 * its own window's round corners ("whiskers", 15 Oct).  The program and
 * the blending are left as the shadows': the caller puts its own back.
 * Returns whether it drew. */
static int shadow_one(uint32_t i, int H, float s)
{
    if (P.scratch || P.bk[i].page || !P.bk[i].nshadow || P.bk[i].sh_strength <= 0 || !P.shadow_prog)
        return 0;
    glUseProgram(P.shadow_prog);
    glUniform1f(glGetUniformLocation(P.shadow_prog, "panelh"), (float)H);
    glUniform1f(glGetUniformLocation(P.shadow_prog, "radius"), P.bk[i].radius * s);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);   /* darken by a: multiply what is there */
    glEnable(GL_SCISSOR_TEST);
    glUniform4f(glGetUniformLocation(P.shadow_prog, "box"),
                P.vx + (P.bk[i].x + P.bk[i].sh_dx) * s, P.vy + (P.bk[i].y + P.bk[i].sh_dy) * s,
                P.bk[i].w * s, P.bk[i].h * s);
    glUniform1f(glGetUniformLocation(P.shadow_prog, "blur"), P.bk[i].sh_blur * s);
    glUniform1f(glGetUniformLocation(P.shadow_prog, "strength"), P.bk[i].sh_strength / 255.0f);
    for (uint32_t k = 0; k < P.bk[i].nshadow; k++) {
        const struct ros_v2_rect *q = &P.bk[i].shadow[k];
        int x0 = (int)floorf(P.vx + q->x0 * s), x1 = (int)ceilf(P.vx + q->x1 * s);
        int y0 = (int)floorf(P.vy + q->y0 * s), y1 = (int)ceilf(P.vy + q->y1 * s);
        if (x1 <= x0 || y1 <= y0)
            continue;
        scissor(x0, H - y1, x1 - x0, y1 - y0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    return 1;
}

static void draw_wayland(int W, int H, float s)
{
    if (P.scratch)
        return;
    struct ros_screen_ext x;
    const struct ros_screen_ext *xp = (const struct ros_screen_ext *)block_at(ROS_SCREEN_EXT);
    uint32_t seq = __atomic_load_n(&xp->seq, __ATOMIC_ACQUIRE);
    memcpy(&x, xp, sizeof x);
    if ((seq & 1) || __atomic_load_n(&xp->seq, __ATOMIC_ACQUIRE) != seq)
        return;                         /* mid-write: the next pass draws */
    if (!x.nwindows)
        return;
    glViewport(0, 0, W, H);
    /* A window with nothing to show yet (just opened, or its program
     * gone) is black: the Wimp neither copies nor redraws its page area
     * when it moves (redraw.c), so screen memory there may hold anything */
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    for (uint32_t k = 0; k < x.nwindows && k < ROS_EXT_WINDOWS && !P.has_pages; k++) {
        int shown = 0;
        for (int i = 0; i < WL_SURFS; i++)
            shown |= WL.surf[i].id && WL.surf[i].window == x.window[k].id && WL.surf[i].buffer &&
                     wl_buf(WL.surf[i].buffer) != NULL;
        if (shown)
            continue;
        for (uint32_t r = 0; r < x.nrects && r < ROS_EXT_RECTS; r++) {
            if (x.rect[r].id != x.window[k].id)
                continue;
            int x0 = (int)floorf(P.vx + x.rect[r].x0 * s), x1 = (int)ceilf(P.vx + x.rect[r].x1 * s);
            int y0 = (int)floorf(P.vy + x.rect[r].y0 * s), y1 = (int)ceilf(P.vy + x.rect[r].y1 * s);
            if (x1 > x0 && y1 > y0) {
                scissor(x0, H - y1, x1 - x0, y1 - y0);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
    }
    scissor_none();
    int any = 0;
    for (int i = 0; i < WL_SURFS; i++)
        any |= WL.surf[i].id && WL.surf[i].buffer;
    if (!any)
        return;
    if (!WL.prog) {
        WL.prog = safe_program(wlsh, "the Wayland windows' program");
        if (!WL.prog)
            return;
    }
    glUseProgram(WL.prog);
    glUniform1f(glGetUniformLocation(WL.prog, "panelh"), (float)H);
    glUniform1i(glGetUniformLocation(WL.prog, "tex"), 3);
    glUniform1i(glGetUniformLocation(WL.prog, "tagholes"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE3);
    /* windows first, then their popups over them */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < WL_SURFS; i++) {
            struct wl_surf *sf = &WL.surf[i];
            if (!sf->id || !sf->buffer || (pass == 0) != (sf->window != 0))
                continue;
            if (pass == 0 && P.has_pages)
                continue;                   /* drawn in the list's order (draw_page) */
            struct wl_buf *b = wl_buf(sf->buffer);
            if (!b || (!b->image && !b->shm))
                continue;
            if (b->shm)
                shm_upload(sf, b);
            else
                glBindTexture(GL_TEXTURE_2D, b->tex);
            glUniform1i(glGetUniformLocation(WL.prog, "bgra"), b->shm != NULL);
            glUniform1i(glGetUniformLocation(WL.prog, "opaque"),
                        b->format == DRM_FORMAT_XRGB8888 || b->format == DRM_FORMAT_XBGR8888);
            float sw = (float)(sf->w ? sf->w : b->w), sh = (float)(sf->h ? sf->h : b->h);
            if (sf->window) {
                /* where the Wimp puts its window, clipped to what is seen */
                int found = 0;
                for (uint32_t k = 0; k < x.nwindows && k < ROS_EXT_WINDOWS; k++)
                    if (x.window[k].id == sf->window) {
                        float ox = x.window[k].x + sf->x, oy = x.window[k].y + sf->y;
                        for (uint32_t r = 0; r < x.nrects && r < ROS_EXT_RECTS; r++)
                            if (x.rect[r].id == sf->window)
                                wl_quad(b, ox, oy, sw, sh, x.rect[r].x0, x.rect[r].y0,
                                        x.rect[r].x1, x.rect[r].y1, H, s);
                        found = 1;
                    }
                (void)found;
            } else {
                /* a popup: wherever it is, on the desktop */
                wl_quad(b, (float)sf->x, (float)sf->y, sw, sh, sf->x, sf->y,
                        sf->x + (int)sw, sf->y + (int)sh, H, s);
            }
        }
    glActiveTexture(GL_TEXTURE0);
    scissor_none();
    glDisable(GL_BLEND);
}

/* ---- the choices ----------------------------------------------------------
 * Choices.Present on the box's disc, a text file of lines "backdrop SPEC",
 * "filter NAME" and "scanlines on|off" (| starts a comment), until block
 * v2's present area carries them from !Appearance (design 37, 3.7).  Read
 * when it changes, looked at once a second; the keys below write it, so
 * what is chosen on the PC survives a restart. */

static const char *const filters[] = { "nearest", "sharp", "linear" };
#define NFILTERS (int)(sizeof filters / sizeof filters[0])
#define WALLPAPERS "/host/Resources/Wallpapers"

/* A relative FILE is in the disc's Wallpapers folder, with or without
 * HostFS's ,ff9; NULL if there is no such file */
static const char *backdrop_resolve(const char *file, char *buf, size_t n)
{
    struct stat st;
    if (file[0] == '/')
        snprintf(buf, n, "%s", file);
    else
        snprintf(buf, n, WALLPAPERS "/%s", file);
    if (stat(buf, &st) == 0)
        return buf;
    size_t l = strlen(buf);
    if (l + 5 < n) {
        strcat(buf, ",ff9");
        if (stat(buf, &st) == 0)
            return buf;
    }
    return NULL;
}

/* ---- the shader scenes ----------------------------------------------------
 * metal.m's scenes (1686-2040), in GLSL ES 3.00: each .metal scene in the
 * disc's Wallpapers.Shaders has a .glsl sibling, the same scene by hand
 * (design 37, 3.5), with the same //! directives.  The file defines
 *
 *     vec3 scene(SceneIn inp)
 *
 * or, with //! feedback, vec4 scene(SceneIn inp, sampler2D previous).
 * The prelude gives SceneIn and the helpers; a scene draws into a
 * texture of its own, at its resolution and no faster than its fps, and
 * the scale pass stretches it under the desktop.  Row 0 of the texture is
 * the scene's top: inp.pos is (0,0) at the top left, y down, as in
 * metal.m, and a feedback scene's state rows lie below its last. */

#define SCENE_MAX_FPS 15.0

static const char *const scene_prelude =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "struct SceneIn {\n"
    "    vec2 pos;      /* this pixel's centre, scene pixels; 0,0 the top left */\n"
    "    vec2 size;     /* the visible scene, in scene pixels */\n"
    "    float scale;   /* scene pixels to a RISC OS pixel */\n"
    "    float time;    /* seconds since the scene was chosen */\n"
    "    float daytime; /* the time of day, in hours, 0 to 24 */\n"
    "    uint frame;    /* frames since the scene began: 0 is the first */\n"
    "    vec3 key;      /* the backdrop key colour, #B7C0B4 */\n"
    "    float iconbar; /* the icon bar's height, in scene pixels */\n"
    "    vec2 guest;    /* the RISC OS screen, in its own pixels */\n"
    "    uint rows;     /* the state rows below the visible scene */\n"
    "};\n"
    "float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
    "vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
    "vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
    /* Metal's fmod truncates; GLSL's mod floors */
    "float fmod(float a, float b) { return a - b * trunc(a / b); }\n"
    "vec2 fmod(vec2 a, float b) { return a - b * trunc(a / b); }\n"
    "float hash12(vec2 p) {\n"
    "    uvec2 q = uvec2(ivec2(floor(p))) * uvec2(1597334673u, 3812015801u);\n"
    "    uint n = (q.x ^ q.y) * 1597334673u;\n"
    "    return float(n >> 8) * (1.0 / 16777216.0);\n"
    "}\n"
    "vec2 hash22(vec2 p) { return vec2(hash12(p), hash12(p + vec2(37.0, 17.0))); }\n"
    "float noise(vec2 p) {\n"
    "    vec2 i = floor(p), f = fract(p);\n"
    "    vec2 u = f * f * (3.0 - 2.0 * f);\n"
    "    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x),\n"
    "               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);\n"
    "}\n"
    "float fbm(vec2 p, int octaves) {\n"
    "    float v = 0.0, a = 0.5;\n"
    "    for (int i = 0; i < octaves; i++) {\n"
    "        v += a * noise(p);\n"
    "        p = mat2(1.6, 1.2, -1.2, 1.6) * p;\n"
    "        a *= 0.5;\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    "vec3 dither(vec2 pos) {\n"
    "    float n = fract(52.9829189 * fract(dot(pos, vec2(0.06711056, 0.00583715))));\n"
    "    return vec3((n - 0.5) / 255.0);\n"
    "}\n"
    "vec4 fetch(sampler2D t, vec2 pos) {\n"
    "    ivec2 hi = textureSize(t, 0) - 1;\n"
    "    return texelFetch(t, clamp(ivec2(floor(pos)), ivec2(0), hi), 0);\n"
    "}\n";

static const char *const scene_postlude =
    "\n"
    "uniform vec4 Ua;      /* size w, h; scale; time */\n"
    "uniform vec4 Ub;      /* daytime; icon bar; guest w, h */\n"
    "uniform vec4 Uc;      /* key r, g, b */\n"
    "uniform uvec4 Ud;     /* frame; state rows */\n"
    "#if BACKDROP_FEEDBACK\n"
    "uniform sampler2D previous_frame;\n"
    "#endif\n"
    "out vec4 scene_colour;\n"
    "void main() {\n"
    "    SceneIn s;\n"
    "    s.pos = gl_FragCoord.xy;\n"
    "    s.size = Ua.xy;\n"
    "    s.scale = Ua.z;\n"
    "    s.time = Ua.w;\n"
    "    s.daytime = Ub.x;\n"
    "    s.iconbar = Ub.y;\n"
    "    s.guest = Ub.zw;\n"
    "    s.key = Uc.xyz;\n"
    "    s.frame = Ud.x;\n"
    "    s.rows = Ud.y;\n"
    "#if BACKDROP_FEEDBACK\n"
    "    scene_colour = scene(s, previous_frame);\n"
    "#else\n"
    "    scene_colour = vec4(saturate(scene(s)), 1.0);\n"
    "#endif\n"
    "}\n";

/* a shader, compiled; 0 with the compiler's message in the log if not */
static GLuint try_shader(GLenum kind, const char *src, const char *what)
{
    GLuint sh = glCreateShader(kind);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof log, &n, log);
        say("%s does not compile:\n%.*s", what, (int)n, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static char *read_text(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *t = malloc(max + 1);
    size_t n = t ? fread(t, 1, max, f) : 0;
    fclose(f);
    if (!t || n == 0 || n == max) {
        free(t);
        return NULL;
    }
    t[n] = 0;
    return t;
}

/* A scene's program from its .glsl file; 0 if it does not build.  The
 * directives are read into *fps, *res, *feedback and *rows. */
static GLuint scene_build(const char *path, float *fps, float *res,
                          int *feedback, int *rows)
{
    char *text = read_text(path, 512 * 1024);
    if (!text) {
        say("backdrop: cannot read the scene %s", path);
        return 0;
    }
    *fps = 12, *res = 1, *feedback = 0, *rows = 0;
    for (char *l = text; l && *l; ) {
        char *e = strchr(l, '\n');
        while (*l == ' ' || *l == '\t')
            l++;
        if (!strncmp(l, "//!", 3)) {
            char k[32] = "", v[32] = "";
            sscanf(l + 3, "%31s %31s", k, v);
            if (!strcasecmp(k, "fps"))
                *fps = fminf(fmaxf((float)atof(v), 0), SCENE_MAX_FPS);
            else if (!strcasecmp(k, "feedback"))
                *feedback = 1;
            else if (!strcasecmp(k, "resolution"))
                *res = !strcasecmp(v, "guest") ? 0 : fminf(fmaxf((float)atof(v), 0.125f), 1);
            else if (!strcasecmp(k, "state"))
                *rows = atoi(v) < 0 ? 0 : atoi(v) > 64 ? 64 : atoi(v);
        }
        l = e ? e + 1 : NULL;
    }
    if (!*feedback)
        *rows = 0;
    size_t n = strlen(scene_prelude) + strlen(text) + strlen(scene_postlude) + 128;
    char *src = malloc(n);
    if (!src) {
        free(text);
        return 0;
    }
    snprintf(src, n, "%s#define BACKDROP_FEEDBACK %d\n#line 1\n%s\n%s", scene_prelude,
             *feedback, text, scene_postlude);
    free(text);
    GLuint fs = try_shader(GL_FRAGMENT_SHADER, src, path);
    free(src);
    if (!fs)
        return 0;
    GLuint vs = try_shader(GL_VERTEX_SHADER, vsh, "the scene's vertex shader");
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei ln = 0;
        glGetProgramInfoLog(p, sizeof log, &ln, log);
        say("backdrop: the scene %s does not link: %.*s", path, (int)ln, log);
        glDeleteProgram(p);
        return 0;
    }
    if (*feedback && !strstr((const char *)glGetString(GL_EXTENSIONS),
                             "GL_EXT_color_buffer_float")) {
        say("backdrop: the scene %s needs float targets, which this GL has not", path);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* The scene's next frame, into its own texture (the other of two, for a
 * feedback scene, which reads the last) */
static void scene_render(void)
{
    if (!P.scn_prog || !P.block.xres)
        return;
    if (P.s <= 0) {                 /* before the first frame placed it */
        float sx = (float)P.mode.hdisplay / P.block.xres;
        float sy = (float)P.mode.vdisplay / P.block.yres;
        P.s = sx < sy ? sx : sy;
    }
    int gw = (int)P.block.xres, gh = (int)P.block.yres;
    int cw = (int)(gw * P.s), ch = (int)(gh * P.s);
    int w = P.scn_res <= 0 ? gw : (int)lroundf(cw * P.scn_res);
    int h = P.scn_res <= 0 ? gh : (int)lroundf(ch * P.scn_res);
    w = w < 1 ? 1 : w > 8192 ? 8192 : w;
    h = h < 1 ? 1 : h > 8192 - P.scn_rows ? 8192 - P.scn_rows : h;
    int n = P.scn_feedback ? 2 : 1;
    if (w != P.scn_w || h != P.scn_h) {
        for (int i = 0; i < n; i++) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, P.scn_tex[i]);
            tex_params(GL_NEAREST);
            if (P.scn_feedback)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h + P.scn_rows, 0,
                             GL_RGBA, GL_FLOAT, NULL);
            else
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, NULL);
            /* a feedback scene's first "last frame" is zero, not junk */
            glBindFramebuffer(GL_FRAMEBUFFER, P.scn_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, P.scn_tex[i], 0);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glActiveTexture(GL_TEXTURE0);
        P.scn_w = w, P.scn_h = h;
        P.scn_cur = 0;
        P.scn_frame = 0;
    }
    time_t tt = time(NULL);
    struct tm lt;
    localtime_r(&tt, &lt);
    float scale = (float)h / (float)gh;
    int target = P.scn_feedback ? P.scn_cur ^ 1 : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, P.scn_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           P.scn_tex[target], 0);
    glViewport(0, 0, w, h + P.scn_rows);
    glUseProgram(P.scn_prog);
    glUniform4f(glGetUniformLocation(P.scn_prog, "Ua"), (float)w, (float)h, scale,
                (float)fmod((now_ms() - P.scn_t0) / 1000.0, 86400.0));
    glUniform4f(glGetUniformLocation(P.scn_prog, "Ub"),
                (float)(lt.tm_hour + lt.tm_min / 60.0 + lt.tm_sec / 3600.0),
                66.0f * scale, (float)gw, (float)gh);
    glUniform4f(glGetUniformLocation(P.scn_prog, "Uc"), 0xB7 / 255.0f, 0xC0 / 255.0f,
                0xB4 / 255.0f, 0);
    glUniform4ui(glGetUniformLocation(P.scn_prog, "Ud"), P.scn_frame,
                 (GLuint)P.scn_rows, 0, 0);
    if (P.scn_feedback) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, P.scn_tex[P.scn_cur]);
        glUniform1i(glGetUniformLocation(P.scn_prog, "previous_frame"), 0);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (P.scn_feedback)
        P.scn_cur ^= 1;
    P.scn_frame++;
    P.scn_valid = 1;
}

/* Whether the scene should draw again now */
static int scene_due(double t)
{
    if (P.bmode != 5 || !P.tagged)
        return 0;                   /* nothing shows it */
    if (!P.scn_valid)
        return 1;
    return P.scn_fps > 0 && t - P.scn_at >= 1000.0 / P.scn_fps;
}

/* The backdrop, by metal.m's spec (backdrop_set_scene):
 *
 *   none            nothing beneath (the desktop's holes stay sage)
 *   acorn           the Acorn mark, built in
 *   acorn-live      the same with two drifting lights
 *   tile:FILE       a sprite file, tiled from the top left
 *   picture:FILE    a sprite file, filling the screen, centred
 *   wallpaper:FILE  a tile if 512 RISC OS pixels or less each way, else
 *                   a picture
 *   FILE            a wallpaper
 *
 * shader: scenes come with P3.  What cannot be read gives none. */
static void backdrop_set(const char *spec)
{
    int mode = 0;
    const char *kind = NULL, *file = NULL;
    if (!*spec || !strcasecmp(spec, "none") || !strcasecmp(spec, "off"))
        spec = "none";
    else if (!strcasecmp(spec, "acorn"))
        mode = 1;
    else if (!strcasecmp(spec, "acorn-live"))
        mode = 2;
    else {
        static const char *const kinds[] = { "tile:", "picture:", "wallpaper:", "shader:" };
        for (int i = 0; i < 4; i++)
            if (!strncasecmp(spec, kinds[i], strlen(kinds[i]))) {
                kind = kinds[i];
                file = spec + strlen(kinds[i]);
            }
        if (!kind) {
            kind = "wallpaper:";
            file = spec;
        }
    }
    if (file) {
        char buf[320];
        const char *path = backdrop_resolve(file, buf, sizeof buf);
        size_t w = 0, h = 0;
        int dpi = 90;
        /* the denser sibling (NAME11) when the desktop is scaled 2x */
        float sx = (float)P.mode.hdisplay / (P.block.xres ? P.block.xres : 1);
        uint8_t *rgba = NULL;
        if (!strcmp(kind, "shader:")) {
            /* the .glsl sibling of a .metal file, in Wallpapers.Shaders */
            char name[256], gp[320];
            snprintf(name, sizeof name, "%s", file);
            char *dot = strrchr(name, '.');
            if (dot && !strcasecmp(dot, ".metal"))
                *dot = 0;
            if (!strchr(name, '.'))
                strcat(name, ".glsl");
            if (name[0] == '/')
                snprintf(gp, sizeof gp, "%s", name);
            else if (!strncasecmp(name, "Shaders/", 8))
                snprintf(gp, sizeof gp, WALLPAPERS "/%s", name);
            else
                snprintf(gp, sizeof gp, WALLPAPERS "/Shaders/%s", name);
            float fps, res;
            int fb, rows;
            GLuint prog = scene_build(gp, &fps, &res, &fb, &rows);
            if (prog) {
                if (P.scn_prog)
                    glDeleteProgram(P.scn_prog);
                P.scn_prog = prog;
                P.scn_fps = fps, P.scn_res = res;
                P.scn_feedback = fb, P.scn_rows = rows;
                P.scn_w = P.scn_h = 0;      /* targets made anew */
                P.scn_valid = 0;
                P.scn_t0 = now_ms();
                mode = 5;
                say("backdrop: scene %s: %.0f fps, resolution %s, feedback %d, "
                    "state rows %d", gp, fps, res > 0 ? "of the screen's" : "a RISC OS "
                    "pixel's", fb, rows);
            }
        } else if (!path)
            say("backdrop %s: no such file", spec);
        else if (!(rgba = box_sprite_load(path, sx >= 2.0f, &w, &h, &dpi)))
            say("backdrop %s: not a sprite file the reader can read", spec);
        if (rgba) {
            P.bg_scale = dpi / 90.0f;
            if (P.bg_scale < 0.25f || P.bg_scale > 8.0f)
                P.bg_scale = 1.0f;
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, P.bg_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glActiveTexture(GL_TEXTURE0);
            free(rgba);
            P.bg_w = (int)w;
            P.bg_h = (int)h;
            if (!strcmp(kind, "tile:"))
                mode = 4;
            else if (!strcmp(kind, "picture:"))
                mode = 3;
            else
                mode = w <= 512 * P.bg_scale && h <= 512 * P.bg_scale ? 4 : 3;
            say("backdrop: %s %s (%zux%zu, %d dpi)", mode == 4 ? "tile" : "picture",
                path, w, h, dpi);
        } else if (!mode) {
            spec = "none";
        }
        if (!mode)
            spec = "none";
    }
    snprintf(P.spec, sizeof P.spec, "%s", spec);
    P.bmode = mode;
}

/* What Ctrl-Alt-F1 steps through: the built-in scenes, then the disc's
 * wallpapers by name */
static int backdrop_list(char names[][64], int max)
{
    int n = 0;
    static const char *const builtin[] = { "none", "acorn", "acorn-live" };
    for (int i = 0; i < 3 && n < max; i++)
        snprintf(names[n++], 64, "%s", builtin[i]);
    DIR *d = opendir(WALLPAPERS);
    struct dirent *e;
    int first = n;
    while (d && (e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.')
            continue;               /* macOS's ._ files, over SMB */
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ",ff9") && l - 4 < 50) {
            snprintf(names[n], 64, "wallpaper:%.*s", (int)(l - 4), e->d_name);
            n++;
        }
    }
    if (d)
        closedir(d);
    qsort(names[first], (size_t)(n - first), 64,
          (int (*)(const void *, const void *))strcasecmp);
    first = n;
    d = opendir(WALLPAPERS "/Shaders");
    while (d && (e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.')
            continue;               /* macOS's ._ files, over SMB */
        if (l > 5 && !strcasecmp(e->d_name + l - 5, ".glsl") && l < 40) {
            snprintf(names[n], 64, "shader:Shaders/%s", e->d_name);
            n++;
        }
    }
    if (d)
        closedir(d);
    qsort(names[first], (size_t)(n - first), 64,
          (int (*)(const void *, const void *))strcasecmp);
    return n;
}

static void backdrop_next(void)
{
    static char names[64][64];
    int n = backdrop_list(names, 64), at = -1;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(names[i], P.spec))
            at = i;
    backdrop_set(names[(at + 1) % n]);
}

static const char *choices_path(int *exists)
{
    static const char *const names[] = { "/host/Choices/Present,fff",
                                         "/host/Choices/Present" };
    struct stat st;
    for (int i = 0; i < 2; i++)
        if (stat(names[i], &st) == 0) {
            *exists = 1;
            return names[i];
        }
    *exists = 0;
    return names[0];        /* a text file, &FFF, as HostFS names it */
}

static int choices(void)
{
    const char *force = getenv("ROSGD_PRESENT_BACKDROP");
    if (force) {
        /* the probe's backdrop, by hand: the disc's choices left alone */
        if (P.choices_read)
            return 0;
        P.choices_read = 1;
        P.filt = 1;
        backdrop_set(force);
        say("choices: backdrop %s (by hand)", P.spec);
        return 1;
    }
    static time_t last;
    time_t now = time(NULL);
    if (P.choices_read && now == last)
        return 0;
    last = now;
    int exists;
    const char *path = choices_path(&exists);
    struct stat st;
    time_t m = exists && stat(path, &st) == 0 ? st.st_mtime : 0;
    if (P.choices_read && m == P.choices_mtime)
        return 0;
    P.choices_mtime = m;
    char spec[192] = "acorn";               /* acorn, sharp-bilinear */
    int filt = 1, scan = 0;
    FILE *f = exists ? fopen(path, "r") : NULL;
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char k[32], v[160];
        if (sscanf(line, "%31s %159s", k, v) != 2 || k[0] == '|' || k[0] == '#')
            continue;
        if (!strcasecmp(k, "backdrop")) {
            snprintf(spec, sizeof spec, "%s", v);
        } else if (!strcasecmp(k, "filter")) {
            for (int i = 0; i < NFILTERS; i++)
                if (!strncasecmp(v, filters[i], strlen(filters[i])))
                    filt = i;
        } else if (!strcasecmp(k, "scanlines")) {
            scan = !strcasecmp(v, "on");
        }
    }
    if (f)
        fclose(f);
    int changed = !P.choices_read || strcasecmp(spec, P.spec) ||
                  filt != P.filt || scan != P.scan;
    if (!P.choices_read || strcasecmp(spec, P.spec))
        backdrop_set(spec);
    P.filt = filt, P.scan = scan;
    P.choices_read = 1;
    if (changed)
        say("choices: backdrop %s, filter %s, scanlines %s",
            P.spec, filters[filt], scan ? "on" : "off");
    return changed;
}

static void choices_write(void)
{
    int exists;
    const char *path = choices_path(&exists);
    mkdir("/host/Choices", 0755);
    FILE *f = fopen(path, "w");
    if (!f) {
        say("choices: %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "| The GPU presenter's choices (design 37): Ctrl-Alt-F1 the\n"
               "| backdrop, Ctrl-Alt-F2 the filter, Ctrl-Alt-F3 scanlines\n"
               "backdrop %s\nfilter %s\nscanlines %s\n",
            P.spec, filters[P.filt], P.scan ? "on" : "off");
    fclose(f);
    struct stat st;
    if (stat(path, &st) == 0)
        P.choices_mtime = st.st_mtime;      /* ours: not read back */
    say("choices: backdrop %s, filter %s, scanlines %s",
        P.spec, filters[P.filt], P.scan ? "on" : "off");
}

/* ---- the keys -------------------------------------------------------------
 * The PC has no host menu, so the presenter's choices have keys: Ctrl-Alt
 * with F1 (the next backdrop), F2 (the next filter) and F3 (scanlines on
 * and off).  The keyboards are read beside RISC OS, never grabbed, so
 * RISC OS sees every key as before.  wperun hides /dev/input as it hides
 * the cards, so the nodes are made here, from sysfs's numbers. */

static void keys_open(void)
{
    mkdir("/dev/input", 0755);
    mount("tmpfs", "/dev/input", "tmpfs", MS_NOSUID, "mode=0755");
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    while (d && (e = readdir(d)) && P.nkbd < 16) {
        if (strncmp(e->d_name, "event", 5))
            continue;
        char sys[96], node[64];
        snprintf(sys, sizeof sys, "/sys/class/input/%s/dev", e->d_name);
        snprintf(node, sizeof node, "/dev/input/%s", e->d_name);
        FILE *f = fopen(sys, "r");
        unsigned maj = 0, min = 0;
        int ok = f && fscanf(f, "%u:%u", &maj, &min) == 2;
        if (f)
            fclose(f);
        if (!ok || (mknod(node, S_IFCHR | 0400, makedev(maj, min)) != 0 &&
                    errno != EEXIST))
            continue;
        int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        /* a keyboard: one with F1 and Ctrl */
        unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) {
            close(fd);
            continue;
        }
#define HAS(k) (bits[(k) / (8 * sizeof(long))] >> ((k) % (8 * sizeof(long))) & 1)
        if (HAS(KEY_F1) && HAS(KEY_LEFTCTRL))
            P.kbd[P.nkbd++] = fd;
        else
            close(fd);
#undef HAS
    }
    if (d)
        closedir(d);
    say("keys: %d keyboard%s watched for Ctrl-Alt-F1/F2/F3", P.nkbd,
        P.nkbd == 1 ? "" : "s");
}

/* 1 when a choice changed */
static int keys(void)
{
    int changed = 0;
    for (int i = 0; i < P.nkbd; i++) {
        struct input_event ev[32];
        ssize_t n;
        while ((n = read(P.kbd[i], ev, sizeof ev)) > 0) {
            for (size_t j = 0; j < (size_t)n / sizeof ev[0]; j++) {
                if (ev[j].type != EV_KEY)
                    continue;
                int down = ev[j].value != 0;
                switch (ev[j].code) {
                case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
                    P.ctrl = down; break;
                case KEY_LEFTALT: case KEY_RIGHTALT:
                    P.alt = down; break;
                case KEY_F4:
                    if (ev[j].value == 1 && P.ctrl && P.alt)
                        P.remodeset = 1;    /* the panel dark: the link trained again */
                    break;
                case KEY_F1: case KEY_F2: case KEY_F3:
                    if (ev[j].value != 1 || !P.ctrl || !P.alt)
                        break;              /* presses, not repeats */
                    if (ev[j].code == KEY_F1)
                        backdrop_next();
                    else if (ev[j].code == KEY_F2)
                        P.filt = (P.filt + 1) % NFILTERS;
                    else
                        P.scan = !P.scan;
                    choices_write();
                    changed = 1;
                    break;
                }
            }
        }
    }
    return changed;
}

/* ---- a frame --------------------------------------------------------------
 * Decode when the screen changed, then scale over the backdrop, then the
 * pointer, then the flip. */

/* this thread's CPU time, ms (Mesa's threads are not the loop's) */
static double cpu_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void draw(int decode, int flat)
{
    const int W = P.mode.hdisplay, H = P.mode.vdisplay;
    float sx = (float)W / P.block.xres, sy = (float)H / P.block.yres;
    float s = sx < sy ? sx : sy;            /* centred, aspect kept */
    P.s = s;
    int cw = (int)(P.block.xres * s), ch = (int)(P.block.yres * s);
    P.vx = (W - cw) / 2;
    P.vy = (H - ch) / 2;

    if (decode) {
        dec_target();
        glBindFramebuffer(GL_FRAMEBUFFER, P.dec_fbo);
        glViewport(0, 0, P.dec_w, P.dec_h);
        glUseProgram(P.dprog);
        glUniform1i(glGetUniformLocation(P.dprog, "pitch"), (GLint)P.block.pitch);
        glUniform1i(glGetUniformLocation(P.dprog, "bpp"), (GLint)P.block.bpp);
        glUniform1i(glGetUniformLocation(P.dprog, "pixo"), (GLint)P.block.pixo);
        glUniform2i(glGetUniformLocation(P.dprog, "pan"),
                    (GLint)P.block.xoffset, (GLint)P.block.yoffset);
        glUniform1i(glGetUniformLocation(P.dprog, "holes"), P.bmode != 0);
        glUniform1i(glGetUniformLocation(P.dprog, "test"), flat);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, P.raw_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, P.pal_tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    /* The clip (partial redraw): this frame's damage, and what the frames
     * since this back buffer was last drawn changed, by its age; the whole
     * panel when the age is unknown or anything asked for all of it */
    {
        int full = P.dmg_full || !P.age_ok;
        EGLint age = 0;
        if (!full && !eglQuerySurface(P.dpy, P.esurf, EGL_BUFFER_AGE_EXT, &age))
            full = 1;
        if (!full && (age <= 0 || age - 1 > P.ring_n))
            full = 1;
        struct pbox r = P.dmg_now;
        for (int k = 0; !full && k < age - 1; k++)
            pbox_add(&r, P.dmg_ring[k]);
        struct pbox rec = P.dmg_full ? (struct pbox){ 0, 0, W, H } : P.dmg_now;
        memmove(&P.dmg_ring[1], &P.dmg_ring[0], 3 * sizeof rec);
        P.dmg_ring[0] = rec;
        if (P.ring_n < 4)
            P.ring_n++;
        if (r.x0 < 0) r.x0 = 0;
        if (r.y0 < 0) r.y0 = 0;
        if (r.x1 > W) r.x1 = W;
        if (r.y1 > H) r.y1 = H;
        P.clip_on = !full && !(r.x0 <= 0 && r.y0 <= 0 && r.x1 >= W && r.y1 >= H);
        if (r.x0 >= r.x1 || r.y0 >= r.y1)
            r = (struct pbox){ 0, 0, 0, 0 };   /* nothing changed in it: a frame of nothing */
        P.clip = r;
        P.dmg_now = PBOX_EMPTY;
        P.dmg_full = 0;
    }
    /* a look: the frame into look_tex, the quantising pass after.  A new
     * texture is drawn whole */
    int look = ROS_LOOK_L2BPP(P.look) >= 0 && P.wimp_only;
    if (look && (P.look_w != W || P.look_h != H)) {
        glBindTexture(GL_TEXTURE_2D, P.look_tex);
        tex_params(GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, P.look_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, P.look_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            P.look_w = W, P.look_h = H;
        } else {
            say("look: no frame texture");
            look = 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        P.clip_on = 0;                          /* all of it, this once */
        P.clip = (struct pbox){ 0, 0, W, H };
    }
    if (look)
        glBindFramebuffer(GL_FRAMEBUFFER, P.look_fbo);
    scissor_none();

    /* the whole panel, border and all: the pointer rides past the
     * content's edge, and what it leaves there must be rubbed out */
    glViewport(0, 0, W, H);
    glUseProgram(P.sprog);
    glUniform2f(glGetUniformLocation(P.sprog, "src"),
                (float)P.block.xres, (float)P.block.yres);
    glUniform4f(glGetUniformLocation(P.sprog, "rect"),
                (float)P.vx, (float)P.vy, (float)cw, (float)ch);
    glUniform1f(glGetUniformLocation(P.sprog, "panelh"), (float)H);
    /* a whole scale is drawn nearest: a native desktop exact to the
     * pixel (3.4), whatever the filter */
    int whole = sx == floorf(sx) && sy == floorf(sy);
    glUniform1i(glGetUniformLocation(P.sprog, "filt"), whole ? 0 : P.filt);
    glUniform1i(glGetUniformLocation(P.sprog, "bmode"), P.bmode);
    glUniform1f(glGetUniformLocation(P.sprog, "tms"),
                (float)fmod(now_ms(), 1800000.0));
    glUniform1f(glGetUniformLocation(P.sprog, "iconbar"), 66.0f);
    glUniform1i(glGetUniformLocation(P.sprog, "scan"), P.scan);
    glUniform1i(glGetUniformLocation(P.sprog, "layer"), P.wimp_only);
    glUniform2f(glGetUniformLocation(P.sprog, "isz"), (float)P.bg_w, (float)P.bg_h);
    glUniform2f(glGetUniformLocation(P.sprog, "bs"), (float)P.scn_w, (float)P.scn_h);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, P.scn_tex[P.scn_feedback ? P.scn_cur : 0]);
    glUniform1f(glGetUniformLocation(P.sprog, "iscale"), s / P.bg_scale);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, P.bg_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, P.dec_tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    draw_backings(W, H, s);                 /* the windows' chrome from their backings (Sprint 3) */
    draw_surfaces(W, H, s);                 /* the icons the GPU draws: over their window's
                                               backing (the icon bar is one), clipped by the Wimp
                                               to what is seen of them */
    draw_wayland(W, H, s);                  /* the Wayland windows, from their buffers */

    if (look) {
        /* the frame shown in the look's colours, through the same clip */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        glDisable(GL_BLEND);
        scissor_none();
        glUseProgram(P.qprog);
        int l2 = ROS_LOOK_L2BPP(P.look);
        glUniform1i(glGetUniformLocation(P.qprog, "l2"), l2);
        glUniform1i(glGetUniformLocation(P.qprog, "grey"), (P.look & ROS_LOOK_GREY) != 0);
        glUniform1i(glGetUniformLocation(P.qprog, "fmt"),
                    (P.look & ROS_LOOK_4K) ? 2 : (P.look & ROS_LOOK_64K) ? 1 : 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, P.look_tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    if (P.cur_quad && P.ptr_shown_now) {
        /* the pointer, in the frame: 64 screen pixels square, at the
         * block's image top left, GL's rows running up -- and clipped to
         * the content's rectangle, for the RISC OS screen ends at its
         * border */
        glEnable(GL_SCISSOR_TEST);
        scissor(P.vx, P.vy, cw, ch);
        int px = (int)(P.ptr_px * s) + P.vx;
        int py = H - ((int)(P.ptr_py * s) + P.vy) - (int)(64.0f * s);
        glViewport(px, py, (int)(64.0f * s), (int)(64.0f * s));
        glUseProgram(P.ptr_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, P.ptr_tex);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
        scissor_none();
    }
    P.clip_on = 0;
    glDisable(GL_SCISSOR_TEST);
}

static void on_signal(int sig)
{
    if (sig == SIGUSR1)
        P.shot = 1;             /* kill -USR1: a readback of the next frame */
    else
        P.run = 0;
}

int main(int argc, char **argv)
{
    int flat = argc > 1 && !strcmp(argv[1], "colour");
    int eglonly = argc > 1 && !strcmp(argv[1], "egl");
    /* scenes: every scene in the disc's Wallpapers.Shaders compiled by this
     * GL, and the verdicts printed -- the display left alone */
    int scenes = argc > 1 && !strcmp(argv[1], "scenes");
    eglonly |= scenes;
    setvbuf(stderr, NULL, _IOLBF, 0);
    /* the pid, for /init: a *Restart's fresh RISC OS retires this one
     * (a probe run beside the presenter must not take its place) */
    if (!(argc > 1 && (!strcmp(argv[1], "egl") || !strcmp(argv[1], "scenes")))) {
        char pid[16];
        snprintf(pid, sizeof pid, "%d\n", (int)getpid());
        int f = open("/run/present.pid", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) {
            write(f, pid, strlen(pid));
            close(f);
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);
    P.run = 1;
    /* wperun's environment carries LIBGL_ALWAYS_SOFTWARE for the
     * browser's sake; Mesa honours it in EGL too, and the presenter
     * would find no device (or a software one) */
    unsetenv("LIBGL_ALWAYS_SOFTWARE");
    /* Gallium's own nouveau driver (nvc0), not Zink over Vulkan: measured
     * on the T1000 with the GSP firmware, glmark2 1306 against Zink's 393
     * (design 37, P0), and the user's decision.  Mesa 25.1 on routes
     * Turing's GL through Zink unless told not to. */
    setenv("NOUVEAU_USE_ZINK", "0", 1);

    if (eglonly)
        setenv("ROSGD_PRESENT_SCRATCH", "1", 1);
    P.card = open_card();
    if (P.card < 0 && !eglonly && !scenes &&
        (access("/dev/dri/card0", F_OK) == 0 || access("/dev/dri/card1", F_OK) == 0)) {
        /* a GPU, but no panel on it yet: the DP link of a panel asleep, or
         * still training after a firmware restart, comes up seconds after
         * nouveau does (seen on the T1000: every connector disconnected,
         * no modes, the panel showing the boot text).  The box's own path
         * would find nothing either, and would hold the screen with no
         * picture; so the display is claimed now and the panel waited for */
        say("no panel connected yet: waiting for one");
        ready();
        while (P.run && (P.card = open_card()) < 0)
            sleep(1);
        if (P.card < 0)
            return 0;
        say("a panel has come");
    }
    if (P.card < 0)
        die("no card with a display connected");
    if (!eglonly && drmSetMaster(P.card) != 0)
        die("cannot be DRM master (someone else has the display)");
    pick_mode();
    open_screen();
    egl_setup();
    gl_setup();
    if (scenes) {
        DIR *d = opendir(WALLPAPERS "/Shaders");
        struct dirent *e;
        int good = 0, all = 0;
        while (d && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (e->d_name[0] == '.')
                continue;           /* macOS's ._ files, over SMB */
            if (l < 6 || strcasecmp(e->d_name + l - 5, ".glsl"))
                continue;
            char path[320];
            snprintf(path, sizeof path, WALLPAPERS "/Shaders/%s", e->d_name);
            float fps, res;
            int fb, rows;
            GLuint p = scene_build(path, &fps, &res, &fb, &rows);
            all++;
            good += p != 0;
            printf("%-24s %s\n", e->d_name, p ? "compiles" : "FAILS (see the log)");
            if (p)
                glDeleteProgram(p);
        }
        if (d)
            closedir(d);
        GLuint cs = try_shader(GL_FRAGMENT_SHADER, clksh, "the clock's program");
        printf("%-24s %s\n", "(the clock surface)", cs ? "compiles" : "FAILS (see the log)");
        all++, good += cs != 0;
        GLuint ms = try_shader(GL_FRAGMENT_SHADER, mtrsh, "the meter's program");
        printf("%-24s %s\n", "(the meter surface)", ms ? "compiles" : "FAILS (see the log)");
        all++, good += ms != 0;
        printf("%d of %d scenes compile\n", good, all);
        return good == all ? 0 : 1;
    }
    cursor_init();

    keys_open();
    wl_listen();                               /* the compositor's socket */
    /* every program built now, before the first frame needs it: a build on
     * first use stalled that frame (the desktop "strangely slow and clunky"
     * at first, 15 Oct) */
    P.clk_prog = safe_program(clksh, "the clock's program");
    P.mtr_prog = safe_program(mtrsh, "the meter's program");
    WL.prog = safe_program(wlsh, "the Wayland windows' program");
    P.shadow_prog = safe_program(shadowsh, "the shadows' program");
    choices();

    struct timespec idle = { 0, 16666667 };   /* the still screen's nap */
    double scene_at = 0;                       /* the backdrop's last frame */
    double minute_at = now_ms();               /* the pacing's report */
    unsigned loops = 0, frames_then = 0, why[4] = { 0 };
    double cpu_scan = 0, cpu_draw = 0, cpu_other = 0;   /* ms, the minute's */
    P.dmg_now = PBOX_EMPTY;
    P.dmg_full = 1;
    double start_at = now_ms(), modeset_check = 0;
    int modeset_done = 0;
    while (P.run) {
        {
            double tm = now_ms();
            if (!modeset_done && P.frame > 0 && tm - start_at > 2500) {
                modeset_done = 1;
                P.remodeset = 1;               /* once after the start */
            }
            if (tm - modeset_check > 1000) {
                modeset_check = tm;
                if (unlink("/run/present-modeset") == 0)
                    P.remodeset = 1;
            }
            if (P.remodeset) {
                P.remodeset = 0;
                remodeset();
            }
        }
        pointer();                             /* outside the frame clock */
        self_update();                         /* a new program on the disc
                                                  takes over at once */
        double c0 = cpu_ms();
        present_beat();                     /* "running", every pass */
        int chose = choices() | keys();
        int surf = surfaces();
        if (wl_poll()) {
            surf = 1;
            dmg_pages();                        /* a page's next frame */
            for (int i = 0; i < WL_SURFS; i++)
                if (WL.surf[i].id && !WL.surf[i].window)
                    dmg_all();                  /* a popup: wherever it went */
        }
        int decode = screen_follow();
        double c1 = cpu_ms();
        decode |= screen_upload() | chose | (P.frame == 0);
        if (decode)
            dmg_all();                          /* the framebuffer, a mode, a choice: all of it */
        {
            /* an icon surface drawn again at the rate it asked for, its box
             * alone (Wimp_Extend &5204's R7; Sprint 5): a clock's smooth
             * hand, no memory copied.  A clock that asked nothing, once a
             * second */
            static double due[ROS_SURFACES];
            double tnow = now_ms();
            for (uint32_t k = 0; k < P.surf.n && k < ROS_SURFACES; k++) {
                uint32_t rate = P.surf_rate[k];
                if (!rate && P.surf.s[k].kind == ROS_SURF_CLOCK)
                    rate = 1;
                if (!rate || tnow < due[k])
                    continue;
                due[k] = tnow + 1000.0 / rate;
                dmg_screen(P.surf.s[k].x, P.surf.s[k].y, P.surf.s[k].x + P.surf.s[k].w,
                           P.surf.s[k].y + P.surf.s[k].h);
                surf = 1;
            }
        }
        v2_ack();
        double c2 = cpu_ms();
        cpu_other += c1 - c0, cpu_scan += c2 - c1;
        /* acorn-live moves: at most 15 frames a second, and only while
         * a hole shows it (metal.m's pacing) */
        double t = now_ms();
        int scene = P.bmode == 2 && P.tagged > 0 && t - scene_at >= 1000.0 / 15;
        if (scene_due(t)) {
            scene_render();             /* a shader scene's next frame */
            P.scn_at = t;
            scene = 1;
        }
        snap_check(t);
        int snap = snap_due(t);
        if (scene)
            dmg_all();                          /* the backdrop moves under everything */
        if (P.ptr_dirty && P.cur_quad) {
            /* the pointer drawn in the frame: where it was and where it is */
            if (P.ptr_box_on)
                pbox_add(&P.dmg_now, P.ptr_box);
            P.ptr_box_on = 0;
            if (P.ptr_shown_now) {
                int x0 = (int)(P.ptr_px * P.s) + P.vx, y0 = (int)(P.ptr_py * P.s) + P.vy;
                int side = (int)(64.0f * P.s) + 2;
                P.ptr_box = (struct pbox){ x0 - 2, y0 - 2, x0 + side, y0 + side };
                pbox_add(&P.dmg_now, P.ptr_box);
                P.ptr_box_on = 1;
            }
        } else if (P.ptr_dirty) {
            dmg_all();                          /* the plane's pointer came or went: rub it out */
        }
        int dirty = decode | P.ptr_dirty | P.shot | scene | surf | snap | caret_check(t);
        /* nothing on the panel changed (a list published with no change
         * that shows): no frame at all, no flip (Sprint 5) */
        if (dirty && !P.dmg_full && P.dmg_now.x0 >= P.dmg_now.x1 && !P.shot && !snap && P.frame > 0)
            dirty = 0;
        /* once a minute, what the frames were for: the idle figure */
        loops++;
        why[0] += decode != 0, why[1] += P.ptr_dirty != 0, why[2] += scene != 0, why[3] += surf != 0;
        if (t - minute_at >= 60000.0) {
            say("pacing: %u frames in the last minute (%u screen, %u pointer, "
                "%u backdrop, %u surfaces), %u passes; cpu ms: scan %.0f, draw %.0f, the "
                "rest %.0f; v2 missed %u, caret window rows %u", (unsigned)P.frame - frames_then,
                why[0], why[1], why[2], why[3], loops, cpu_scan, cpu_draw, cpu_other, P.missed,
                P.caret_fixed);
            P.missed = 0;
            P.caret_fixed = 0;
            P.missed_said = 0;
            minute_at = t, frames_then = (unsigned)P.frame, loops = 0;
            memset(why, 0, sizeof why);
            cpu_scan = cpu_draw = cpu_other = 0;
        }
        P.ptr_dirty = 0;
        if (dirty) {
            if (scene)
                scene_at = t;
            double d0 = cpu_ms();
            draw(decode, flat);
            if (P.frame == 0 || P.shot) {
                P.shot = 0;
                screenshot();               /* before the swap: read the draw */
            }
            if (snap)
                snap_take(t);               /* the burst's frame, likewise */
            if (P.scratch) {
                /* the probe has no display: one frame drawn, read, done */
                say("egl probe: drawn and read back");
                return 0;
            }
            eglSwapBuffers(P.dpy, P.esurf);
            flip_and_wait(P.frame == 0);
            wl_flipped();                    /* frame done, buffers given back */
            cpu_draw += cpu_ms() - d0;
            if (P.frame == 1 && !P.scratch)
                ready();                     /* /init's three seconds end */
        } else {
            /* nothing changed: look again in a frame's time.  (The one in
             * six full check, and the pointer's own path outside the
             * frame, follow with the cursor plane.) */
            nanosleep(&idle, NULL);
        }
    }
    say("%d frames drawn", P.frame);
    return 0;
}
