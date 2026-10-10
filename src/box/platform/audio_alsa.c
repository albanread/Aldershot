/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* audio_alsa.c -- the box's sound: 16-bit stereo from the kernel's ALSA
 * PCM, filled by one callback (platform.h), which SharedSound's driver
 * SWI will be.
 *
 * It follows DRM's pattern. Linux owns the hardware (an intel-hda on
 * QEMU's line, played by the host's backend) and this file is the whole of
 * the box's side. There is no library: the PCM is driven by the kernel's
 * own interface (uapi sound/asound.h, which zig's libc carries), with
 * open, ioctl and write as DRM is. A missing card (the hosted build, or a
 * box without one on its line) is not a failure. The pump thread still
 * runs at the system rate and the callback still fires, into a discarded
 * buffer, so SharedSound and its clients never know.
 *
 * On the HAL (BOX on QEMU without Linux) there is no ALSA. The HAL's call
 * HAL_SYS_AUDIO gives a descriptor that plays 16-bit stereo at 44.1 kHz
 * through virtio-sound, written to and paced as a PCM is, with PREPARE and
 * DROP as its only ioctls. Linux answers -ENOSYS.
 */
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#include "rosgd/platform.h"

#define RATE        44100                 /* the system's: SharedSound's */
#define PERIOD_MS   20                    /* one fill every 20 ms */
#define FRAMES      (RATE / 1000 * PERIOD_MS)
#define CARD_MAX    96000                 /* the most a card's rate may be */
#define AHEAD_NS    160000000L            /* the most the pump runs ahead of the
                                           * clock: the card's buffer, 160 ms */

static int pcm = -1;
static int pcm_card = -1;                       /* the card and device it is on: the */
static int pcm_device = -1;                     /* mixer's, and *ListSpeakers' "playing" */
/* *SelectSpeaker asks and the pump does it. Only the pump ever opens or
 * closes the card, so there is no lock for the box's own thread to wait
 * on. Waiting on one would stop the box, since the thread that would
 * release it is the one filling the sound. */
static volatile int want_card = -1, want_device = -1;
static volatile int want_state = ROS_AUDIO_SELECT_DONE;
static pthread_t thread;
static int running;
static int16_t buffer[FRAMES][2];

/* A card that will not play 44.1 kHz plays the system's sound converted to
 * its own rate. (VZ's virtio-snd offers 48 to 96 kHz.) Each 20 ms fill
 * (882 frames) becomes the card's 20 ms (960 at 48 kHz), linearly
 * interpolated, with the last frame of one fill carried into the next */
static unsigned card_rate = RATE, card_frames = FRAMES;
static int16_t converted[CARD_MAX / 1000 * PERIOD_MS][2];
static int16_t carried[2];

/* What goes to the card, kept as a WAV of 16-bit stereo at the card's
 * rate, to hear what the card is given before the host's audio has it.
 * rosgd.soundcapture=N takes the first N seconds at the start.
 * *SoundCapture (modules/speakers) takes N seconds whenever it is asked,
 * which is the only way on a machine that cannot be rebooted with another
 * command line. The file is the share's SoundCapture.wav, or the disc's
 * where there is no share. The card's underruns are also counted and
 * reported as they happen. */
static uint8_t *capture;                 /* the WAV, built in memory */
static size_t capture_at, capture_size;
static char capture_path[256];

static void le32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x, p[1] = (uint8_t)(x >> 8), p[2] = (uint8_t)(x >> 16), p[3] = (uint8_t)(x >> 24);
}

/* the WAV's header, for 16-bit stereo at rate and `bytes` of sound */
static void wav_header(uint8_t *h, uint32_t rate, uint32_t bytes)
{
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le32(h + 20, 1 | 2u << 16);          /* PCM, stereo */
    le32(h + 24, rate);
    le32(h + 28, rate * 4);
    le32(h + 32, 4 | 16u << 16);         /* 4 bytes a frame, 16 bits */
    memcpy(h + 36, "data", 4);
    le32(h + 40, bytes);
}

/* The capture is written to the share in one go, when it is full. Writing
 * as it goes held up the desktop's start with the pump's file writes. */
static void capture_save(void)
{
    wav_header(capture, card_rate, (uint32_t)(capture_at - 44));
    if (!capture_path[0]) {
        /* The share, or the disc when a box has none (a PC's). /host is
         * there either way, because it is where the share mounts. So what says
         * a share is really on it is that it is a different file system
         * from the root's. */
        struct stat host, root;
        int shared = !stat("/host", &host) && !stat("/", &root) && host.st_dev != root.st_dev;
        snprintf(capture_path, sizeof capture_path, "%s/SoundCapture.wav",
                 shared ? "/host" : "/disc");
    }
    int fd = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t done = 0;
    while (fd >= 0 && done < capture_at) {
        ssize_t n = write(fd, capture + done, capture_at - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    if (fd >= 0)
        close(fd);
    ros_console_printf("rosgd: sound: capture done, %zu bytes to %s%s\n",
                       done, capture_path, done == capture_at ? "" : " -- the write failed");
    free(capture);
    capture = NULL;
}
static unsigned underruns;

/* The callback chain: fill `frames` interleaved stereo frames, the buffer
 * zeroed first so each filler adds.  NULL head is silence. */
#define FILL_MAX 4
static ros_audio_fill *fills[FILL_MAX];
static unsigned nfills;

void ros_audio_start(ros_audio_fill *fn)
{
    fills[0] = fn;
    nfills = fn ? 1 : 0;
}

void ros_audio_add(ros_audio_fill *fn)
{
    if (fn && nfills < FILL_MAX)
        fills[nfills++] = fn;
}

void ros_audio_remove(ros_audio_fill *fn)
{
    for (unsigned i = 0; i < nfills; i++) {
        if (fills[i] != fn)
            continue;
        for (unsigned j = i + 1; j < nfills; j++)
            fills[j - 1] = fills[j];
        nfills--;
        break;
    }
}

int ros_audio_rate(void)
{
    return RATE;
}

static atomic_int muted;

void ros_audio_mute(int on)
{
    atomic_store(&muted, on != 0);
}

int ros_audio_muted(void)
{
    return atomic_load(&muted);
}

static struct ros_audio_meter meter;     /* the pump's alone to write */
static uint64_t last_loud;               /* the last fill's frames not silent */

void ros_audio_meter(struct ros_audio_meter *m)
{
    *m = meter;                         /* a snapshot, each word whole */
}

static int emit(int16_t *out, size_t frames)
{
    memset(out, 0, frames * 4);
    for (unsigned i = 0; i < nfills; i++)
        fills[i](out, frames);
    if (atomic_load(&muted))
        memset(out, 0, frames * 4);     /* *Speaker Off: the meter sees it */
    uint64_t loud = 0, full = 0;
    uint32_t peak = meter.peak;
    for (size_t i = 0; i < frames * 2; i += 2) {
        int32_t l = out[i], r = out[i + 1];
        uint32_t a = (uint32_t)(l < 0 ? -l : l), b = (uint32_t)(r < 0 ? -r : r);
        loud += (a | b) != 0;
        full += a >= 32766 || b >= 32766;
        if (a > peak)
            peak = a;
        if (b > peak)
            peak = b;
    }
    meter.loud += loud;
    last_loud = loud;
    meter.full += full;
    meter.peak = peak;
    meter.frames += frames;
    return last_loud != 0;
}

/* the interval at index n of hw_params (0 = ACCESS, a mask; the rest
 * intervals) */
static struct snd_interval *ival(struct snd_pcm_hw_params *p, unsigned n)
{
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Set the card for 16-bit stereo at `rate`, a period of 20 ms and a
 * buffer of a few: 0, or the card's errno */
static int pcm_set(int fd, unsigned rate)
{
    /* everything allowed first, as alsa-lib's snd_pcm_hw_params_any: a
     * mask or interval left zero is empty, and the card refuses the lot */
    struct snd_pcm_hw_params p;
    memset(&p, 0, sizeof p);
    for (unsigned n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(&p.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK], 0xff, sizeof p.masks[0]);
    for (unsigned n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++)
        ival(&p, n)->max = ~0u;
    p.rmask = ~0u;
    p.info = ~0u;
    struct snd_mask *a = &p.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(a, 0, sizeof *a);
    a->bits[SNDRV_PCM_ACCESS_RW_INTERLEAVED / 32] = 1u << (SNDRV_PCM_ACCESS_RW_INTERLEAVED % 32);
    struct snd_mask *f = &p.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(f, 0, sizeof *f);
    f->bits[SNDRV_PCM_FORMAT_S16_LE / 32] = 1u << (SNDRV_PCM_FORMAT_S16_LE % 32);
    struct snd_interval *c = ival(&p, SNDRV_PCM_HW_PARAM_CHANNELS);
    c->min = c->max = 2;
    c->integer = 1;
    struct snd_interval *r = ival(&p, SNDRV_PCM_HW_PARAM_RATE);
    r->min = r->max = rate;
    r->integer = 1;
    unsigned frames = rate / 1000 * PERIOD_MS;
    struct snd_interval *s = ival(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    s->min = frames / 2;                        /* the card may halve it */
    s->max = frames;
    s->integer = 1;
    /* The whole buffer is up to 160 ms, so that what is filled is heard
     * soon but a host that is late taking it (VZ playing to a real output)
     * does not starve the card. The write below blocks once it is full,
     * and that is the pump's clock. */
    struct snd_interval *b = ival(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    b->min = frames;
    b->max = frames * 8;
    b->integer = 1;
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0)
        return errno;

    /* Never stop for an underrun, and play silence in any gap. VZ's
     * virtio-snd, playing to a real output, takes each period into its own
     * buffer at once, so to Linux the card has run dry after every write.
     * Stopping there, and PREPARE to start again, every 20 ms, was a buzz
     * over everything (silence too). The stop threshold at the boundary
     * (alsa-lib's, as aplay sets it) keeps the stream running. The
     * silence size at the boundary fills what is played with silence, so
     * a gap is never old sound played again. */
    snd_pcm_uframes_t buffer = ival(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    snd_pcm_uframes_t period = ival(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    snd_pcm_uframes_t boundary = buffer;
    while (boundary * 2 <= (snd_pcm_uframes_t)LONG_MAX - buffer)
        boundary *= 2;
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof sw);
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period;
    sw.start_threshold = period;
    sw.stop_threshold = boundary;
    sw.silence_threshold = 0;
    sw.silence_size = boundary;
    sw.boundary = boundary;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0)
        ros_console_printf("rosgd: sound: the card refused its software parameters (%s): "
                           "it stops at each underrun\n", strerror(errno));
    ros_console_printf("rosgd: sound: %u Hz%s, periods of %u frames, a buffer of %u\n", rate,
                       rate == RATE ? "" : " (the system's 44100 converted)",
                       ival(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min, ival(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min);
    return 0;
}

/* One card's playback device: 16-bit stereo at the first rate it takes */
static int pcm_try(unsigned card, unsigned device)
{
    char path[64];
    snprintf(path, sizeof path, "/dev/snd/pcmC%uD%up", card, device);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    /* the system's rate if the card has it, else the nearest above that a
     * 20 ms fill converts to whole frames */
    static const unsigned rates[] = { RATE, 48000, 88200, 96000 };
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        if (pcm_set(fd, rates[i]) != 0)
            continue;
        card_rate = rates[i];
        card_frames = card_rate / 1000 * PERIOD_MS;
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
        pcm_card = (int)card, pcm_device = (int)device;
        return fd;
    }
    close(fd);
    return -1;
}

/* A display that will take sound. An HDMI or DisplayPort output is a
 * sound device of its own, and the kernel says which of them has a monitor
 * that declares audio in its EDID: /proc/asound/card<c>/eld#<codec>.<pin>,
 * with monitor_present and eld_valid both 1, and the monitor's name in it.
 * A card's eld files are its outputs in order, and so are its HDMI devices
 * in /proc/asound/pcm, which is how a pin becomes a device number.
 *
 * This is looked at first, because a monitor that says it takes sound is
 * somewhere the sound certainly goes, while the board's analogue socket
 * with nothing plugged into it is silence. A PC on a desk is far more
 * often a screen with speakers than a pair of speakers on a jack.
 * rosgd.sound=<card>,<device> settles it the other way. */
static const char *eld_value(const char *line, const char *key)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) || (line[n] != ' ' && line[n] != '\t'))
        return NULL;
    const char *v = line + n;
    while (*v == ' ' || *v == '\t')
        v++;
    return v;
}

/* What one output of a card is, from its eld file: 1 when a monitor is
 * there and says it takes sound, with its name and how it is connected */
static int eld_of(unsigned card, unsigned pin, char *name, size_t nmax, char *how, size_t hmax)
{
    for (unsigned codec = 0; codec < 4; codec++) {
        char path[96];
        snprintf(path, sizeof path, "/proc/asound/card%u/eld#%u.%u", card, codec, pin);
        FILE *e = fopen(path, "r");
        if (!e)
            continue;
        int present = 0, valid = 0;
        char line[160];
        while (fgets(line, sizeof line, e)) {
            const char *v;
            if ((v = eld_value(line, "monitor_present")) != NULL)
                present = atoi(v);
            else if ((v = eld_value(line, "eld_valid")) != NULL)
                valid = atoi(v);
            else if ((v = eld_value(line, "monitor_name")) != NULL)
                snprintf(name, nmax, "%s", v);
            else if ((v = eld_value(line, "connection_type")) != NULL)
                snprintf(how, hmax, "%s", v);
        }
        fclose(e);
        name[strcspn(name, "\r\n")] = 0;
        how[strcspn(how, "\r\n")] = 0;
        return present && valid;
    }
    return 0;
}

/* Every output the box could play through. /proc/asound/pcm lists the
 * cards' playback devices: the board's own, and one per display output
 * of every graphics card. A card's eld files are its display outputs in
 * the same order, which is how a pin becomes a device number. */
unsigned ros_audio_outputs(struct ros_audio_output *out, unsigned max)
{
    FILE *f = fopen("/proc/asound/pcm", "r");
    if (!f)
        return 0;
    unsigned n = 0, displays[4] = { 0, 0, 0, 0 };
    char line[256];
    while (n < max && fgets(line, sizeof line, f)) {
        unsigned card, device;
        if (sscanf(line, "%u-%u:", &card, &device) != 2 || card >= 4 || !strstr(line, "playback"))
            continue;
        const char *what = strchr(line, ':');
        for (what = what ? what + 1 : ""; *what == ' '; what++)
            ;
        struct ros_audio_output *o = &out[n];
        memset(o, 0, sizeof *o);
        o->card = card, o->device = device, o->live = 1;
        snprintf(o->name, sizeof o->name, "%s", what);
        char *end = strstr(o->name, " : ");         /* the id, not the rest of the line */
        if (end)
            *end = 0;
        o->name[strcspn(o->name, "\r\n")] = 0;
        if (!strncmp(o->name, "HDMI", 4) || !strncmp(o->name, "DP", 2)) {
            char monitor[64] = "", how[24] = "";
            o->live = eld_of(card, displays[card]++, monitor, sizeof monitor, how, sizeof how);
            if (o->live && monitor[0])
                snprintf(o->name, sizeof o->name, "%s", monitor);
            snprintf(o->how, sizeof o->how, "%s", how[0] ? how : "a display output");
        }
        o->playing = (int)card == pcm_card && (int)device == pcm_device;
        n++;
    }
    fclose(f);
    return n;
}

static void mixer_set(void);

/* The box plays through another output from now on: *SelectSpeaker asks
 * here, and the pump makes the change between two fills (take_request).
 * The caller watches ros_audio_select_state for the answer. */
void ros_audio_select(unsigned card, unsigned device)
{
    want_card = (int)card, want_device = (int)device;
    want_state = ROS_AUDIO_SELECT_ASKED;
}

int ros_audio_select_state(void)
{
    return want_state;
}

/* The pump, between fills: the card asked for, or the one it has kept */
static void take_request(void)
{
    if (want_state != ROS_AUDIO_SELECT_ASKED)
        return;
    int was = pcm, was_card = pcm_card, was_device = pcm_device;
    int fd = pcm_try((unsigned)want_card, (unsigned)want_device);
    if (fd < 0) {
        pcm_card = was_card, pcm_device = was_device;
        want_state = ROS_AUDIO_SELECT_REFUSED;
        return;
    }
    pcm = fd;
    if (was >= 0)
        close(was);
    mixer_set();
    want_state = ROS_AUDIO_SELECT_DONE;
}

/* A display with a monitor that will take sound. It is looked at before
 * the board's own socket, because a monitor that says it takes sound is
 * somewhere the sound certainly goes, while an analogue socket with
 * nothing plugged into it is silence. A PC on a desk is more often a
 * screen with speakers than a pair of speakers on a jack. *SelectSpeaker,
 * or rosgd.sound=<card>,<device>, settles it the other way. */
static int display_device(unsigned *card_out, unsigned *device_out, char *name, size_t max)
{
    struct ros_audio_output list[16];
    unsigned n = ros_audio_outputs(list, 16);
    for (unsigned i = 0; i < n; i++)
        if (list[i].how[0] && list[i].live) {
            *card_out = list[i].card, *device_out = list[i].device;
            snprintf(name, max, "%s on %s", list[i].name, list[i].how);
            return 1;
        }
    return 0;
}

/* The card to play through.  A PC has more than one: the board's own, and
 * every graphics card, whose devices are the display outputs (card 1
 * D3, D7, D8, D9 on an NVIDIA).  Device 0 is the analogue output, so that
 * is looked for first, on each card in turn; failing that, anything that
 * will take 16-bit stereo, which is how a monitor's speakers on HDMI get
 * the sound when a machine has nothing else.  rosgd.sound=<card>,<device>
 * names one instead, and rosgd.sound=off asks for silence. */
#define HAL_SYS_AUDIO 0x7201

static int pcm_open(void)
{
    const char *want = ros_cmdline_value("rosgd.sound");
    if (want && !strcmp(want, "off"))
        return -1;
    uint32_t hal[2];                            /* the HAL's: its descriptor and rate */
    if (syscall(HAL_SYS_AUDIO, hal) == 0) {
        card_rate = hal[1];
        card_frames = card_rate / 1000 * PERIOD_MS;
        pcm_card = pcm_device = 0;
        ros_console_printf("rosgd: sound: the HAL's virtio-sound, %u Hz\n", card_rate);
        return (int)hal[0];
    }
    if (want) {
        unsigned card = 0, device = 0;
        char *end;
        card = (unsigned)strtoul(want, &end, 10);
        if (*end == ',')
            device = (unsigned)strtoul(end + 1, NULL, 10);
        int fd = pcm_try(card, device);
        if (fd < 0)
            ros_console_printf("rosgd: sound: card %u device %u will not play 16-bit stereo "
                               "(%s): silence\n", card, device, strerror(errno));
        return fd;
    }
    unsigned card, device;
    char monitor[64];
    if (display_device(&card, &device, monitor, sizeof monitor)) {
        int fd = pcm_try(card, device);
        if (fd >= 0) {
            ros_console_printf("rosgd: sound: %s takes it (card %u device %u); "
                               "*ListSpeakers for the others\n", monitor, card, device);
            return fd;
        }
    }
    for (card = 0; card < 4; card++) {
        int fd = pcm_try(card, 0);
        if (fd >= 0)
            return fd;
    }
    for (card = 0; card < 4; card++)
        for (device = 1; device < 10; device++) {
            int fd = pcm_try(card, device);
            if (fd >= 0)
                return fd;
        }
    return -1;                                  /* no card: silence */
}

/* buffer's FRAMES at the system's rate into converted's card_frames at
 * the card's: frame j is at FRAMES * j / card_frames of the fill, one
 * frame late so the carried one is its first neighbour */
static void convert(void)
{
    for (unsigned j = 0; j < card_frames; j++) {
        uint32_t at = (uint32_t)((uint64_t)FRAMES * j * 65536 / card_frames);
        unsigned i = at >> 16;
        int32_t frac = (int32_t)(at & 0xffff);
        for (unsigned ch = 0; ch < 2; ch++) {
            int32_t x0 = i ? buffer[i - 1][ch] : carried[ch], x1 = buffer[i][ch];
            converted[j][ch] = (int16_t)(x0 + (int32_t)(((int64_t)(x1 - x0) * frac) >> 16));
        }
    }
    carried[0] = buffer[FRAMES - 1][0];
    carried[1] = buffer[FRAMES - 1][1];
}

/* The pump. With a card, the blocking write is the clock. It takes a
 * period once the buffer is full, as the card plays one, so the fills keep
 * the card's pace exactly, and the queue's beat with them (#62: a sleep
 * after the write as well made the fills half the card's rate, the beat
 * slow and the card starved). With none, an absolute 20 ms deadline
 * keeps the system rate, so a box without a card keeps time too. */
static void *pump(void *arg)
{
    ros_thread_name("sound");      /* named for /proc: what uses the time */
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    /* Power: after two seconds of nothing but silence the card's stream is
     * stopped (DROP), so that the host's sound hardware can sleep. The fills
     * go on, on the system's clock as with no card, so the beat and the
     * queue keep time. The first fill with a sound in it starts the
     * stream again (PREPARE: the write starts it). */
    enum { PARK_AFTER = 2000 / PERIOD_MS };
    unsigned silent = 0;
    int parked = 0;
    while (running) {
        take_request();                         /* *SelectSpeaker, between fills */
        int sound = emit(&buffer[0][0], FRAMES);
        if (pcm >= 0) {
            silent = sound ? 0 : silent + 1;
            if (parked && (sound || capture)) {     /* a capture records the card's stream */
                ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE);
                parked = 0;
            } else if (!parked && silent > PARK_AFTER && !capture) {
                ioctl(pcm, SNDRV_PCM_IOCTL_DROP);
                parked = 1;
            }
        }
        if (pcm >= 0 && !parked) {
            const int16_t *at = &buffer[0][0];
            size_t left = FRAMES;
            if (card_rate != RATE) {
                convert();
                at = &converted[0][0];
                left = card_frames;
            }
            if (capture) {
                size_t n = left * 4 < capture_size - capture_at ? left * 4 : capture_size - capture_at;
                memcpy(capture + capture_at, at, n);
                if ((capture_at += n) == capture_size)
                    capture_save();
            }
            while (left) {
                ssize_t n = write(pcm, at, left * 4);
                if (n < 0) {
                    if (errno == EPIPE) {       /* underrun: prepare again */
                        if (++underruns <= 10 || underruns % 100 == 0)
                            ros_console_printf("rosgd: sound: underrun %u\n", underruns);
                        ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE);
                        continue;
                    }
                    break;
                }
                at += n / 4;
                left -= n / 4;
            }
        }
        /* The next fill is on the system's clock. With no card (or one that
         * failed) it is exact. With a card, it is exact only if the card
         * has let the pump run more than a buffer ahead, because a card
         * that takes every write at once, as VZ's can, must not hurry the
         * beat. A pump far behind (the box stalled) starts the clock again
         * from now. */
        next.tv_nsec += (long)FRAMES * 1000000000L / RATE;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        if (pcm >= 0 && !parked) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t ahead = (int64_t)(next.tv_sec - now.tv_sec) * 1000000000 + (next.tv_nsec - now.tv_nsec);
            if (ahead < -1000000000)
                next = now;
            if (ahead <= AHEAD_NS)
                continue;
            struct timespec until = next;
            until.tv_nsec -= AHEAD_NS;
            while (until.tv_nsec < 0) {
                until.tv_nsec += 1000000000L;
                until.tv_sec--;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
            continue;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

/* ---- the mixer --------------------------------------------------------------
 *
 * A card's own amplifiers start where the kernel leaves them, and on real
 * hardware that is silence. A PC's ALC222 came up with Speaker and
 * Headphone at volume 0 and the line output muted, while the box played
 * into it quite happily (the stream RUNNING, the pointer advancing).
 * Nothing else in the box's world sets them, because RISC OS's volume is
 * SharedSound's, inside the stream. So the box sets them as it opens the
 * card: every playback switch on, and every playback volume at rosgd.volume
 * per cent of its range (80 by default; "keep" leaves the card alone).
 * Capture, monitoring and anything else are not touched.
 */
static const char *const mixer_outputs[] = {
    "Master", "PCM", "Speaker", "Headphone", "Front", "Surround",
    "Center", "LFE", "Side", "Line Out", "Analog Output",
};

static int mixer_is_output(const char *name)
{
    if (!strstr(name, " Playback "))
        return 0;
    for (size_t i = 0; i < sizeof mixer_outputs / sizeof mixer_outputs[0]; i++) {
        size_t n = strlen(mixer_outputs[i]);
        if (!strncmp(name, mixer_outputs[i], n) && name[n] == ' ')
            return 1;
    }
    return 0;
}

static void mixer_set(void)
{
    const char *vol = ros_cmdline_value("rosgd.volume");
    if (pcm_card < 0 || (vol && !strcmp(vol, "keep")))
        return;
    unsigned pct = vol ? (unsigned)strtoul(vol, NULL, 10) : 80;
    if (pct > 100)
        pct = 100;
    char path[64];
    snprintf(path, sizeof path, "/dev/snd/controlC%d", pcm_card);
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return;
    struct snd_ctl_card_info card;
    memset(&card, 0, sizeof card);
    ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &card);
    struct snd_ctl_elem_list list;
    memset(&list, 0, sizeof list);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || !list.count) {
        close(fd);
        return;
    }
    struct snd_ctl_elem_id *ids = calloc(list.count, sizeof *ids);
    if (!ids) {
        close(fd);
        return;
    }
    unsigned space = list.count;
    memset(&list, 0, sizeof list);
    list.space = space;
    list.pids = ids;
    int set = 0;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0)
        for (unsigned i = 0; i < list.used; i++) {
            if (ids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER ||
                !mixer_is_output((const char *)ids[i].name))
                continue;
            struct snd_ctl_elem_info info;
            memset(&info, 0, sizeof info);
            info.id = ids[i];
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 ||
                !(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE))
                continue;
            long value;
            if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN)
                value = 1;                              /* a switch: on */
            else if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER)
                value = info.value.integer.min +
                        (info.value.integer.max - info.value.integer.min) * (long)pct / 100;
            else
                continue;                               /* an enumeration is a choice, not a level */
            struct snd_ctl_elem_value v;
            memset(&v, 0, sizeof v);
            v.id = ids[i];
            for (unsigned c = 0; c < info.count && c < 128; c++)
                v.value.integer.value[c] = value;
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) == 0)
                set++;
        }
    free(ids);
    close(fd);
    if (set)
        ros_console_printf("rosgd: sound: %s, %d playback control%s on at %u%%\n",
                           card.name[0] ? (const char *)card.name : "a card", set,
                           set == 1 ? "" : "s", pct);
    else
        ros_console_printf("rosgd: sound: %s has no playback controls to set "
                           "(a digital output carries its own level)\n",
                           card.name[0] ? (const char *)card.name : "the card");
}

/* *SoundCapture: take `seconds` of what goes to the card from now, into
 * `path` (or the share's, else the disc's, SoundCapture.wav when it is
 * empty).  0, or -1 when one is already running or there is no room. */
int ros_audio_capture(unsigned seconds, const char *path)
{
    if (!seconds || capture)
        return -1;
    size_t want = 44 + (size_t)seconds * card_rate * 4;
    uint8_t *mem = malloc(want);
    if (!mem)
        return -1;
    snprintf(capture_path, sizeof capture_path, "%s", path && *path ? path : "");
    capture_size = want;
    capture_at = 44;
    capture = mem;                                      /* the pump fills it */
    return 0;
}

int ros_audio_init(void)
{
    pcm = pcm_open();               /* a missing card is silence, not error */
    if (pcm >= 0)
        mixer_set();
    else
        ros_console_printf("rosgd: sound: no card plays 16-bit stereo: silence\n");
    const char *cap = ros_cmdline_value("rosgd.soundcapture");
    if (cap && pcm >= 0) {
        capture_size = 44 + (size_t)strtoul(cap, NULL, 10) * card_rate * 4;
        capture_at = 44;
        capture = malloc(capture_size);
        ros_console_printf("rosgd: sound: capturing %s s at %u Hz, for the share's SoundCapture.wav%s\n",
                           cap, card_rate, capture ? "" : " -- no memory for it");
    }
    running = 1;
    return pthread_create(&thread, NULL, pump, NULL);
}

void ros_audio_final(void)
{
    if (!running)
        return;
    running = 0;
    pthread_join(thread, NULL);
    if (pcm >= 0) {
        ioctl(pcm, SNDRV_PCM_IOCTL_DRAIN);
        close(pcm);
        pcm = -1;
    }
}
