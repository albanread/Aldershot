/* ramfb.c. This file drives the screen through QEMU's ramfb device: a single
 * XRGB8888 framebuffer in guest RAM, which QEMU displays exactly as it is.
 *
 * The HAL tells ramfb the address and size of its framebuffer by writing to
 * the fw_cfg file "etc/ramfb". On the virt machine fw_cfg's own registers
 * are at &09020000, and only DMA can write a fw_cfg file. The framebuffer is
 * one contiguous run of physical pages, which the HAL allocates once, at the
 * size that rosgd.hal.screen=WxH gives (or 1280 x 800 if it is not given).
 * QEMU's window takes that size.
 *
 * The box receives the framebuffer as a memfd over the same pages (through
 * HAL_SYS_SCREEN), and maps it in the same way as a DRM dumb buffer.
 * platform/display_drm.c converts screen memory into it, as it does for any
 * display that shows only what it is given. The box need not tell the HAL
 * when the contents change, because QEMU reads the framebuffer at each of
 * its own refreshes. */
#include "hal.h"

#define FWCFG_PA      0x09020000UL
#define FW_FILE_DIR   0x19
#define DMA_ERROR     0x01
#define DMA_READ      0x02
#define DMA_SELECT    0x08
#define DMA_WRITE     0x10
#define XRGB8888      0x34325258u                       /* 'XR24' */

#define MAX_W 3840
#define MAX_H 2160

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }

static struct {
    uint32_t control, length;
    uint64_t address;
} dma __attribute__((aligned(16)));

/* Perform one DMA transfer. Returns 0, or -1 if fw_cfg reports an error. */
static int fw_dma(uint32_t control, void *buf, uint32_t len)
{
    dma.control = be32(control);
    dma.length = be32(len);
    dma.address = be64(va_to_pa(buf));
    uint64_t pa = va_to_pa(&dma);
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_w32(FWCFG_PA + 16, be32((uint32_t)(pa >> 32)));
    mmio_w32(FWCFG_PA + 20, be32((uint32_t)pa));        /* writing the low half starts the DMA */
    uint32_t c;
    while ((c = be32(*(volatile uint32_t *)&dma.control)) & ~DMA_ERROR)
        ;
    return c & DMA_ERROR ? -1 : 0;
}

/* Return the fw_cfg selector of the named file, or -1 if there is none */
static int fw_file(const char *name)
{
    static struct {
        uint32_t size;
        uint16_t select, reserved;
        char name[56];
    } entry;
    static uint32_t count;
    if (fw_dma(FW_FILE_DIR << 16 | DMA_SELECT | DMA_READ, &count, 4))
        return -1;
    for (uint32_t i = be32(count); i; i--) {
        if (fw_dma(DMA_READ, &entry, sizeof entry))
            return -1;
        if (!strcmp(entry.name, name))
            return __builtin_bswap16(entry.select);
    }
    return -1;
}

static uint32_t number(const char **p)
{
    uint32_t n = 0;
    while (**p >= '0' && **p <= '9')
        n = n * 10 + (uint32_t)(*(*p)++ - '0');
    return n;
}

static struct hal_screen screen;
static uint64_t screen_pa;
static int screen_obj = -1;

int ramfb_screen(struct hal_screen *s)
{
    if (screen_obj >= 0) {
        *s = screen;
        return screen_obj;
    }
    int sel = fw_file("etc/ramfb");
    if (sel < 0)
        return -19;                                     /* ENODEV: QEMU had no -device ramfb */
    uint32_t w = 1280, h = 800;
    const char *a = strstr(boot.bootargs, "rosgd.hal.screen=");
    if (a) {
        a += 17;
        uint32_t x = number(&a), y = *a == 'x' ? (a++, number(&a)) : 0;
        if (x >= 320 && y >= 200 && x <= MAX_W && y <= MAX_H)
            w = x & ~7u, h = y;
    }
    uint32_t pitch = w * 4;
    uint64_t bytes = ((uint64_t)pitch * h + PAGE_MASK) & ~PAGE_MASK;
    if (!(screen_pa = pages_alloc_run(bytes >> 12)))
        return -12;                                     /* ENOMEM */
    static struct __attribute__((packed)) {
        uint64_t addr;
        uint32_t fourcc, flags, width, height, stride;
    } cfg;
    cfg.addr = be64(screen_pa);
    cfg.fourcc = be32(XRGB8888);
    cfg.flags = 0;
    cfg.width = be32(w);
    cfg.height = be32(h);
    cfg.stride = be32(pitch);
    if (fw_dma((uint32_t)sel << 16 | DMA_SELECT | DMA_WRITE, &cfg, sizeof cfg))
        return -5;                                      /* EIO */
    int obj = memfd_obj_phys(screen_pa, bytes);
    if (obj < 0)
        return obj;
    screen = (struct hal_screen){ -1, w, h, pitch, 0 };
    kprintf("HAL: screen %ux%u (ramfb) at %lx\n", w, h, screen_pa);
    *s = screen;
    return screen_obj = obj;
}
