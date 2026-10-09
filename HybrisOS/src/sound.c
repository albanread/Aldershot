/* sound.c. This file provides the box's sound output through the first
 * stream of virtio-sound, as 16-bit stereo at the box's own rate of 44.1 kHz.
 *
 * The box asks for sound with the HAL's call HAL_SYS_AUDIO. (Linux returns
 * -ENOSYS for this call, and audio_alsa.c then goes on to use ALSA.) The HAL
 * gives the box a file descriptor, and the box writes its buffer fills to it
 * in the same way as it writes to an ALSA PCM device. Each write copies one
 * period (20 ms of sound) into one of eight transfer buffers. If the device
 * holds all eight, the write waits. The device returns each buffer when it
 * has played it, so the box's writes keep pace with the device, as they do
 * with ALSA's blocking write.
 *
 * The HAL uses two of virtio-sound's queues: queue 0 for control and queue 2
 * for transmit. It does not use the event and receive queues. */
#include "hal.h"

#define PERIOD   3528                           /* 20 ms: 882 frames of 4 bytes each */
#define SLOTS    8
#define QSIZE    64

enum {
    PCM_SET_PARAMS = 0x0101, PCM_PREPARE = 0x0102, PCM_RELEASE = 0x0103,
    PCM_START = 0x0104, S_OK = 0x8000,
};

static uint64_t base;
static unsigned slot_irq;
static struct vq ctlq, txq;
static uint8_t *ctl;                            /* one page: request at 0, reply at 2048 */
static uint64_t ctl_pa;
static uint8_t *slots;                          /* a page each: header at 0, data at 16, status at 4080 */
static uint64_t slots_pa;
static int busy[SLOTS];
static unsigned next_slot;
static int started;

/* Send one control request and wait for it. Returns 0, or -EIO. */
static long control(const void *req, uint32_t len)
{
    memcpy(ctl, req, len);
    memset(ctl + 2048, 0, 4);
    ctlq.desc[0] = (struct vdesc){ ctl_pa, len, VDESC_NEXT, 1 };
    ctlq.desc[1] = (struct vdesc){ ctl_pa + 2048, 4, VDESC_WRITE, 0 };
    uint16_t want = (uint16_t)(ctlq.used_seen + 1);
    virtio_offer(&ctlq, 0);
    virtio_notify(base, 0);
    while (ctlq.used[1] != want)
        __asm__ volatile("yield" ::: "memory");
    ctlq.used_seen = want;
    uint32_t code;
    memcpy(&code, ctl + 2048, 4);
    return code == S_OK ? 0 : -5;
}

static long stream(uint32_t code)
{
    uint32_t r[2] = { code, 0 };                /* the header, for stream 0 */
    return control(r, sizeof r);
}

/* Mark the transmit buffers that the device has played as free again */
static int reclaim(void)
{
    int got = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    while (txq.used_seen != txq.used[1]) {
        const volatile uint32_t *e = (const volatile uint32_t *)(txq.used + 2) + 2 * (txq.used_seen % QSIZE);
        unsigned d = e[0] % QSIZE;
        txq.used_seen++;
        if (d / 3 < SLOTS)
            busy[d / 3] = 0;
        got = 1;
    }
    return got;
}

int sound_attach(uint64_t b, unsigned n)
{
    if (base)
        return -1;
    uint64_t c = page_alloc(), s = pages_alloc_run(SLOTS);
    if (!c || !s || virtio_begin(b, 0) || virtio_queue(b, 0, &ctlq, 8) || virtio_queue(b, 2, &txq, QSIZE))
        return -1;
    virtio_go(b);
    base = b, slot_irq = n;
    ctl = pa_to_va(c), ctl_pa = c;
    slots = pa_to_va(s), slots_pa = s;
    struct __attribute__((packed)) {
        uint32_t code, stream, buffer_bytes, period_bytes, features;
        uint8_t channels, format, rate, pad;
    } p = { PCM_SET_PARAMS, 0, PERIOD * SLOTS, PERIOD, 0, 2, 5, 6, 0 };   /* format S16, rate 44100 */
    if (control(&p, sizeof p) || stream(PCM_PREPARE)) {
        kprintf("HAL: sound: the device would not take 16-bit stereo at 44.1 kHz\n");
        base = 0;
        return -1;
    }
    spi_enable(VIRTIO_IRQ0 + n);
    kprintf("HAL: sound: virtio-sound, 16-bit stereo at 44.1 kHz\n");
    return 0;
}

int sound_irq(unsigned n)
{
    if (!base || n != slot_irq)
        return 0;
    mmio_w32(base + 0x064, mmio_r32(base + 0x060));
    if (reclaim())
        futex_wake((uint64_t)&busy, 1L << 30);
    return 1;
}

int sound_present(void)
{
    return base != 0;
}

/* Handle a write from the box. This copies up to one period into a free
 * transfer buffer, or waits for a buffer to become free. */
long sound_write(struct frame *fr, uint64_t buf, uint64_t n)
{
    if (!base)
        return -5;
    reclaim();
    if (busy[next_slot]) {
        if (!fr)
            return -11;
        thread_block_restart(fr, (uint64_t)&busy, 0);
        return SWITCHED;
    }
    unsigned i = next_slot;
    uint32_t c = n < PERIOD ? (uint32_t)n & ~3u : PERIOD;
    if (!c)
        return 0;
    uint8_t *p = slots + (uint64_t)i * PAGE_SIZE;
    uint64_t pa = slots_pa + (uint64_t)i * PAGE_SIZE;
    uint32_t id = 0;
    memcpy(p, &id, 4);
    if (copy_from_box(p + 16, buf, c))
        return -14;
    txq.desc[3 * i] = (struct vdesc){ pa, 4, VDESC_NEXT, (uint16_t)(3 * i + 1) };
    txq.desc[3 * i + 1] = (struct vdesc){ pa + 16, c, VDESC_NEXT, (uint16_t)(3 * i + 2) };
    txq.desc[3 * i + 2] = (struct vdesc){ pa + 4080, 8, VDESC_WRITE, 0 };
    busy[i] = 1;
    next_slot = (i + 1) % SLOTS;
    virtio_offer(&txq, (uint16_t)(3 * i));
    virtio_notify(base, 2);
    if (!started) {
        stream(PCM_START);
        started = 1;
    }
    return c;
}

/* The sound pump's ioctls, PREPARE and DROP, need no action here. The HAL
 * starts the stream at the first write and leaves it running. When the
 * pump stops writing (after two seconds of silence), the device has no
 * buffers, so it plays silence, and it plays the next buffers as they
 * arrive. (QEMU's virtio-sound did not play again after a STOP and a
 * START.) */
long sound_ioctl(uint32_t req)
{
    (void)req;
    return 0;
}
