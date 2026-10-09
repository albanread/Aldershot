/* virtio.c: the virtio-mmio transports of QEMU's virt machine, and the
 * virtio-input devices on them (the keyboard and the tablet).
 *
 * virt has 32 transports, &200 bytes apart starting at &0A000000, each
 * with its own interrupt (SPI 16 + n, which is INTID 48 + n).  The HAL
 * drives only the modern interface (version 2); run/run-hal.sh asks QEMU
 * for it.
 *
 * A virtio-input device sends evdev's own events (type, code, value) on
 * its event queue, into buffers that the driver supplies.  This file keeps
 * each event as a Linux struct input_event, stamped with the time it
 * arrived, and the box reads the events from /dev/input/eventN as it does
 * under Linux (syscall.c).  The box's input_evdev.c therefore needs no
 * change.  The device's name and the ranges of its axes are in its
 * configuration space.
 *
 * This file hands a virtio-9p device (the share) to ninep.c, which uses
 * the transport functions here. */
#include "hal.h"

#define MMIO_PA     0x0A000000UL
#define MMIO_STRIDE 0x200
#define MMIO_SLOTS  32
#define MMIO_IRQ0   48

/* The transport's registers */
#define R_MAGIC        0x000
#define R_VERSION      0x004
#define R_DEVICE_ID    0x008
#define R_DEV_FEAT     0x010
#define R_DEV_FEAT_SEL 0x014
#define R_DRV_FEAT     0x020
#define R_DRV_FEAT_SEL 0x024
#define R_QUEUE_SEL    0x030
#define R_QUEUE_MAX    0x034
#define R_QUEUE_NUM    0x038
#define R_QUEUE_READY  0x044
#define R_QUEUE_NOTIFY 0x050
#define R_IRQ_STATUS   0x060
#define R_IRQ_ACK      0x064
#define R_STATUS       0x070
#define R_DESC_LO      0x080
#define R_DESC_HI      0x084
#define R_DRIVER_LO    0x090
#define R_DRIVER_HI    0x094
#define R_DEVICE_LO    0x0A0
#define R_DEVICE_HI    0x0A4
#define R_CONFIG       0x100

#define ID_9P 9
#define ID_NET 1
#define ID_SOUND 25
#define ID_GPU 16
#define S_ACK 1
#define S_DRIVER 2
#define S_DRIVER_OK 4
#define S_FEATURES_OK 8

#define ID_INPUT 18
#define QSIZE 64

struct vevent {                                 /* virtio_input_event */
    uint16_t type, code;
    uint32_t value;
};

struct kevent {                                 /* Linux's struct input_event */
    uint64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};

#define RING 512
static struct input {
    uint64_t base;                              /* physical address of its registers */
    int slot;
    char name[64];
    int32_t abs[2][6];                          /* for X and Y: value, min, max, fuzz, flat, res */
    struct vq q;
    struct vevent *bufs;
    struct kevent ring[RING];
    unsigned head, count;
} inputs[8];
static int ninputs;

/* Read the configuration space: write select and subsel, then read the size
 * and the data, which starts at offset 8. */
static unsigned cfg_read(struct input *d, uint8_t select, uint8_t subsel, void *out, unsigned max)
{
    uint64_t c = d->base + R_CONFIG;
    mmio_w8(c, select);
    mmio_w8(c + 1, subsel);
    unsigned size = mmio_r8(c + 2);
    if (size > max)
        size = max;
    for (unsigned i = 0; i < size; i++)
        ((uint8_t *)out)[i] = mmio_r8(c + 8 + i);
    return size;
}

void spi_enable(unsigned id)
{
    uint64_t gicd = 0x08000000UL;
    uint64_t grp = gicd + 0x080 + 4 * (id / 32);
    mmio_w32(grp, mmio_r32(grp) | 1u << (id % 32));                 /* group 1 */
    uint64_t pri = gicd + 0x400 + (id & ~3u);
    mmio_w32(pri, (mmio_r32(pri) & ~(0xFFu << 8 * (id % 4))) | 0x80u << 8 * (id % 4));
    mmio_w64(gicd + 0x6000 + 8 * id, 0);                            /* route to core 0 */
    mmio_w32(gicd + 0x100 + 4 * (id / 32), 1u << (id % 32));        /* enable */
}

/* ---- The transport ----------------------------------------------------- */

int virtio_begin(uint64_t base, uint32_t want)
{
    mmio_w32(base + R_STATUS, 0);
    mmio_w32(base + R_STATUS, S_ACK | S_DRIVER);
    mmio_w32(base + R_DEV_FEAT_SEL, 1);
    if (!(mmio_r32(base + R_DEV_FEAT) & 1))    /* VIRTIO_F_VERSION_1 */
        return -1;
    mmio_w32(base + R_DEV_FEAT_SEL, 0);
    uint32_t has = mmio_r32(base + R_DEV_FEAT);
    mmio_w32(base + R_DRV_FEAT_SEL, 0);
    mmio_w32(base + R_DRV_FEAT, has & want);
    mmio_w32(base + R_DRV_FEAT_SEL, 1);
    mmio_w32(base + R_DRV_FEAT, 1);
    mmio_w32(base + R_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
    return mmio_r32(base + R_STATUS) & S_FEATURES_OK ? 0 : -1;
}

int virtio_queue(uint64_t base, unsigned q, struct vq *vq, uint16_t size)
{
    mmio_w32(base + R_QUEUE_SEL, q);
    if (mmio_r32(base + R_QUEUE_MAX) < size)
        return -1;
    uint64_t pd = page_alloc(), pa = page_alloc(), pu = page_alloc();
    if (!pd || !pa || !pu)
        return -1;
    memset(vq, 0, sizeof *vq);
    vq->desc = pa_to_va(pd);
    vq->avail = pa_to_va(pa);
    vq->used = pa_to_va(pu);
    vq->size = size;
    mmio_w32(base + R_QUEUE_NUM, size);
    mmio_w32(base + R_DESC_LO, (uint32_t)pd);
    mmio_w32(base + R_DESC_HI, (uint32_t)(pd >> 32));
    mmio_w32(base + R_DRIVER_LO, (uint32_t)pa);
    mmio_w32(base + R_DRIVER_HI, (uint32_t)(pa >> 32));
    mmio_w32(base + R_DEVICE_LO, (uint32_t)pu);
    mmio_w32(base + R_DEVICE_HI, (uint32_t)(pu >> 32));
    mmio_w32(base + R_QUEUE_READY, 1);
    return 0;
}

void virtio_go(uint64_t base)
{
    mmio_w32(base + R_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
}

/* Offer the descriptor chain that starts at head to the device. */
void virtio_offer(struct vq *vq, uint16_t head)
{
    vq->avail[2 + vq->avail_idx % vq->size] = head;
    __asm__ volatile("dmb ish" ::: "memory");
    vq->avail[1] = ++vq->avail_idx;
}

void virtio_notify(uint64_t base, unsigned q)
{
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_w32(base + R_QUEUE_NOTIFY, q);
}

/* ---- virtio-input ------------------------------------------------------- */

static int input_start(struct input *d)
{
    if (virtio_begin(d->base, 0) || virtio_queue(d->base, 0, &d->q, QSIZE))   /* the event queue */
        return -1;
    uint64_t pb = page_alloc();
    if (!pb)
        return -1;
    d->bufs = pa_to_va(pb);
    for (uint16_t i = 0; i < QSIZE; i++) {
        d->q.desc[i] = (struct vdesc){ pb + i * sizeof(struct vevent), sizeof(struct vevent), VDESC_WRITE, 0 };
        virtio_offer(&d->q, i);
    }

    cfg_read(d, 1, 0, d->name, sizeof d->name - 1);         /* VIRTIO_INPUT_CFG_ID_NAME */
    for (int axis = 0; axis < 2; axis++)                    /* CFG_ABS_INFO: X, Y */
        cfg_read(d, 0x12, (uint8_t)axis, &d->abs[axis][1], 20);

    virtio_go(d->base);
    virtio_notify(d->base, 0);
    spi_enable(MMIO_IRQ0 + (unsigned)d->slot);
    return 0;
}

void virtio_init(void)
{
    for (int n = 0; n < MMIO_SLOTS && ninputs < (int)(sizeof inputs / sizeof inputs[0]); n++) {
        uint64_t base = MMIO_PA + (uint64_t)n * MMIO_STRIDE;
        uint32_t id = mmio_r32(base + R_DEVICE_ID);
        if (mmio_r32(base + R_MAGIC) != 0x74726976 || (id != ID_INPUT && id != ID_9P && id != ID_NET && id != ID_SOUND && id != ID_GPU))
            continue;
        if (mmio_r32(base + R_VERSION) != 2) {
            kprintf("HAL: virtio-mmio %d is the legacy interface: not driven\n", n);
            continue;
        }
        if (id == ID_9P) {
            ninep_attach(base);
            continue;
        }
        if (id == ID_GPU) {
            gpu_attach(base);
            continue;
        }
        if (id == ID_SOUND) {
            sound_attach(base, (unsigned)n);
            continue;
        }
        if (id == ID_NET) {
            net_attach(base, (unsigned)n);
            continue;
        }
        struct input *d = &inputs[ninputs];
        memset(d, 0, sizeof *d);
        d->base = base, d->slot = n;
        if (input_start(d) == 0) {
            kprintf("HAL: input %d: %s\n", ninputs, d->name);
            ninputs++;
        }
    }
}

/* Handle an interrupt from transport slot n by collecting the events the
 * device has sent. */
int virtio_irq(unsigned n)
{
    for (int k = 0; k < ninputs; k++) {
        struct input *d = &inputs[k];
        if (d->slot != (int)n)
            continue;
        mmio_w32(d->base + R_IRQ_ACK, mmio_r32(d->base + R_IRQ_STATUS));
        uint64_t f = counter_freq(), c = counter_read();
        int got = 0;
        __asm__ volatile("dmb ish" ::: "memory");
        while (d->q.used_seen != d->q.used[1]) {
            const volatile uint32_t *e = (const volatile uint32_t *)(d->q.used + 2) + 2 * (d->q.used_seen % QSIZE);
            uint16_t id = (uint16_t)e[0];
            struct vevent v = d->bufs[id % QSIZE];
            d->q.used_seen++;
            if (d->count < RING) {
                struct kevent *k2 = &d->ring[(d->head + d->count++) % RING];
                k2->sec = c / f, k2->usec = (c % f) * 1000000 / f;
                k2->type = v.type, k2->code = v.code, k2->value = (int32_t)v.value;
            }
            virtio_offer(&d->q, id % QSIZE);
            got = 1;
        }
        virtio_notify(d->base, 0);
        return got;
    }
    return 0;
}

/* ---- The box's side: /dev/input/eventN ------------------------------- */

int input_devices(void)
{
    return ninputs;
}

const char *input_name(int i)
{
    return inputs[i].name;
}

const int32_t *input_absinfo(int i, int axis)
{
    return inputs[i].abs[axis];
}

int input_pending(int i)
{
    return inputs[i].count != 0;
}

/* Copy as many whole events as fit in n bytes into buf, which is in the
 * box.  Returns the number of bytes copied, or -EAGAIN if there are none. */
long input_read(int i, uint64_t buf, uint64_t n)
{
    struct input *d = &inputs[i];
    if (!d->count)
        return -11;
    uint64_t done = 0;
    while (d->count && n - done >= sizeof(struct kevent)) {
        if (copy_to_box(buf + done, &d->ring[d->head], sizeof(struct kevent)))
            return done ? (long)done : -14;
        d->head = (d->head + 1) % RING, d->count--;
        done += sizeof(struct kevent);
    }
    return done ? (long)done : -22;
}
