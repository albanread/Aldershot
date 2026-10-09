/* gpu.c. This file drives the screen through virtio-gpu in 2D mode. It
 * provides a framebuffer the size of the box's current screen mode, and
 * QEMU's window resizes itself to match.
 *
 * ramfb has a single framebuffer, fixed when the HAL starts. With virtio-gpu,
 * the HAL tells the device each time the box changes mode. It creates a new
 * resource of the new size in XRGB8888 (which virtio-gpu calls B8G8R8X8; the
 * byte order is the same). One contiguous run of pages backs the resource,
 * and the box receives those pages as a memfd (through HAL_SYS_SCREEN, which
 * passes the size the box wants). The HAL then sets the resource as the
 * scanout. The device does not read guest memory by itself. When the box
 * reports a change (HAL_SYS_FLUSH, called from display_drm.c's converter
 * with the rows it wrote), the HAL copies the changed area to the host's
 * copy of the resource and shows it, using TRANSFER_TO_HOST_2D and
 * RESOURCE_FLUSH.
 *
 * The HAL uses only the control queue. It sends one request at a time, and
 * waits for each to complete before it sends the next. */
#include "hal.h"

enum {
    GET_DISPLAY_INFO = 0x0100, RESOURCE_CREATE_2D, RESOURCE_UNREF, SET_SCANOUT, RESOURCE_FLUSH,
    TRANSFER_TO_HOST_2D, RESOURCE_ATTACH_BACKING, RESOURCE_DETACH_BACKING,
    OK_NODATA = 0x1100, OK_DISPLAY_INFO,
};
#define B8G8R8X8 2

struct hdr {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t ctx;
    uint8_t ring, pad[3];
};
struct rect {
    uint32_t x, y, w, h;
};

static uint64_t base;
static struct vq ctlq;
static uint8_t *req;                            /* one page: request at 0, reply at 2048 */
static uint64_t req_pa;
static uint32_t pref_w = 1280, pref_h = 800;    /* the display's preferred size (GET_DISPLAY_INFO) */

static struct {
    uint32_t id, w, h;
    uint64_t pa, pages;
    int obj;
} cur;

static uint32_t command(const void *r, uint32_t len)
{
    memcpy(req, r, len);
    memset(req + 2048, 0, 512);
    ctlq.desc[0] = (struct vdesc){ req_pa, len, VDESC_NEXT, 1 };
    ctlq.desc[1] = (struct vdesc){ req_pa + 2048, 512, VDESC_WRITE, 0 };
    uint16_t want = (uint16_t)(ctlq.used_seen + 1);
    virtio_offer(&ctlq, 0);
    virtio_notify(base, 0);
    while (ctlq.used[1] != want)
        __asm__ volatile("yield" ::: "memory");
    ctlq.used_seen = want;
    mmio_w32(base + 0x064, mmio_r32(base + 0x060));
    return ((const struct hdr *)(req + 2048))->type;
}

int gpu_attach(uint64_t b)
{
    if (base)
        return -1;
    uint64_t r = page_alloc();
    if (!r || virtio_begin(b, 0) || virtio_queue(b, 0, &ctlq, 8))
        return -1;
    virtio_go(b);
    base = b;
    req = pa_to_va(r), req_pa = r;
    struct hdr h = { GET_DISPLAY_INFO, 0, 0, 0, 0, { 0 } };
    if (command(&h, sizeof h) == OK_DISPLAY_INFO) {
        const uint32_t *d = (const uint32_t *)(req + 2048 + sizeof h);  /* pmodes[0]: rectangle, then enabled flag */
        if (d[4] && d[2] >= 320 && d[3] >= 200)
            pref_w = d[2], pref_h = d[3];
    }
    kprintf("HAL: screen: virtio-gpu, the display %ux%u\n", pref_w, pref_h);
    return 0;
}

int gpu_present(void)
{
    return base != 0;
}

/* Show a framebuffer of w x h. Returns the memfd object that covers it, or
 * -errno. */
int gpu_screen(uint32_t w, uint32_t h, struct hal_screen *s)
{
    if (!w || !h)
        w = pref_w, h = pref_h;
    if (w > 3840 || h > 2160 || w < 64 || h < 64)
        return -22;
    if (cur.id && cur.w == w && cur.h == h) {
        *s = (struct hal_screen){ -1, w, h, w * 4, 1 };
        return cur.obj;
    }
    uint64_t bytes = ((uint64_t)w * h * 4 + PAGE_MASK) & ~PAGE_MASK;
    uint64_t pa = pages_alloc_run(bytes >> 12);
    if (!pa)
        return -12;
    uint32_t id = cur.id + 1;
    struct { struct hdr hd; uint32_t id, format, w, h; } c = { { RESOURCE_CREATE_2D, 0, 0, 0, 0, { 0 } }, id, B8G8R8X8, w, h };
    struct { struct hdr h; uint32_t id, n; uint64_t addr; uint32_t len, pad; } a = {
        { RESOURCE_ATTACH_BACKING, 0, 0, 0, 0, { 0 } }, id, 1, pa, (uint32_t)bytes, 0 };
    struct { struct hdr h; struct rect r; uint32_t scanout, id; } so = { { SET_SCANOUT, 0, 0, 0, 0, { 0 } }, { 0, 0, w, h }, 0, id };
    if (command(&c, sizeof c) != OK_NODATA || command(&a, sizeof a) != OK_NODATA ||
        command(&so, sizeof so) != OK_NODATA) {
        for (uint64_t i = 0; i < bytes >> 12; i++)
            page_free(pa + i * PAGE_SIZE);
        return -5;
    }
    int obj = memfd_obj_phys(pa, bytes);
    if (obj < 0)
        return obj;
    if (cur.id) {                               /* release the previous mode's resource */
        struct { struct hdr h; uint32_t id, pad; } d = { { RESOURCE_DETACH_BACKING, 0, 0, 0, 0, { 0 } }, cur.id, 0 };
        command(&d, sizeof d);
        d.h.type = RESOURCE_UNREF;
        command(&d, sizeof d);
        memfd_obj_trim(cur.obj, 0, 1);          /* free its pages; the box has unmapped them */
    }
    cur.id = id, cur.w = w, cur.h = h, cur.pa = pa, cur.pages = bytes >> 12, cur.obj = obj;
    *s = (struct hal_screen){ -1, w, h, w * 4, 1 };
    return obj;
}

/* Copy the rectangle the box has written to the host, and show it */
long gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!cur.id)
        return -19;
    if (x >= cur.w || y >= cur.h)
        return 0;
    if (w > cur.w - x)
        w = cur.w - x;
    if (h > cur.h - y)
        h = cur.h - y;
    counts.screen_flushes++, counts.screen_bytes += (uint64_t)w * h * 4;
    struct { struct hdr h; struct rect r; uint64_t off; uint32_t id, pad; } t = {
        { TRANSFER_TO_HOST_2D, 0, 0, 0, 0, { 0 } }, { x, y, w, h }, ((uint64_t)y * cur.w + x) * 4, cur.id, 0 };
    struct { struct hdr h; struct rect r; uint32_t id, pad; } f = { { RESOURCE_FLUSH, 0, 0, 0, 0, { 0 } }, { x, y, w, h }, cur.id, 0 };
    return command(&t, sizeof t) == OK_NODATA && command(&f, sizeof f) == OK_NODATA ? 0 : -5;
}
