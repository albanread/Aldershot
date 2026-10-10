/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* platform.h -- the HAL, for a RISC OS that runs on Linux.
 *
 * RISC OS 5's HAL abstracts a board: interrupt controllers, timers, the
 * display controller, the IIC bus. Under ROSGD the board is Linux's
 * problem, and what is left is a small layer that turns Linux's
 * interfaces into the functions the personality needs:
 *
 *   console   the serial console, where the runtime reports
 *   display   a DRM dumb buffer on virtio-gpu, mapped into the arena as
 *             screen memory, so compiled VDU code writes it directly
 *   input     evdev, from virtio-input: the source of KeyV and PointerV
 *   time      the monotonic clock, in RISC OS's centiseconds
 *   power     off, at the end of an automated run
 */
#ifndef ROSGD_PLATFORM_H
#define ROSGD_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* ---- console ------------------------------------------------------------ */

void ros_console_init(void);
void ros_console_write(const char *s, size_t n);
void ros_console_putc(int ch);
void ros_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A character; -1 if none came in timeout_ms (-1 waits forever); -2 at
 * the console's end (EOF). */
int ros_console_getc(int timeout_ms);

/* ---- threads -------------------------------------------------------------
 *
 * A thread's name, for /proc and for a debugger: which of the box's
 * threads is using the time.  Linux takes it through prctl, macOS (the
 * hosted build) through pthread_setname_np; both name the calling thread
 * and both truncate at 16 bytes, the terminator counted, so the names
 * given here are short.
 */
void ros_thread_name(const char *name);

/* ---- audio ---------------------------------------------------------------
 *
 * Sound is 16-bit stereo at the system rate, filled by one callback on
 * the platform's own thread (platform/audio_alsa.c; a null sink hosted
 * and cardless).  SharedSound's driver SWI is the callback's module-side
 * face: everything from it down happens there, one baton.
 */
typedef void ros_audio_fill(int16_t *out, size_t frames);   /* interleaved stereo */

int ros_audio_init(void);
void ros_audio_final(void);
void ros_audio_start(ros_audio_fill *fn);

/* The outputs a box could play through, which are the board's own socket
 * and one per display output of every graphics card, and the one it plays
 * through now. The Speakers module (*ListSpeakers, *SelectSpeaker) is
 * these two calls with a RISC OS face on them. A hosted box has none. */
struct ros_audio_output {
    unsigned card, device;      /* the kernel's numbering, as the names say */
    char name[64];              /* "ALC222 Analog", or a monitor's "LG HDR 4K" */
    char how[24];               /* a display's connection ("HDMI", "DisplayPort"); "" otherwise */
    int live;                   /* a display with a monitor that takes sound; 1 for the rest */
    int playing;                /* the one the box is playing through */
};

/* What is driving the screen: the DRM driver's name, and whether the box's
 * screen memory is scanned out as it is (*MachineInfo) */
const char *ros_display_driver(int *direct);

unsigned ros_audio_outputs(struct ros_audio_output *out, unsigned max);

/* Asking for another output is not doing it: the thread filling the sound
 * makes the change between two fills, so that nothing of the box's ever
 * waits on it.  ros_audio_select_state says how far it has got. */
#define ROS_AUDIO_SELECT_DONE     0
#define ROS_AUDIO_SELECT_ASKED    1
#define ROS_AUDIO_SELECT_REFUSED  (-1)

/* What the card is being given, kept as a WAV: *SoundCapture's, for
 * finding out whether the box is making a sound at all */
int ros_audio_capture(unsigned seconds, const char *path);

void ros_audio_select(unsigned card, unsigned device);
int ros_audio_select_state(void);
/* The chain under the one callback: SharedSound mixes its handlers, and the
 * Sound module's voices mix over whatever came before, each in turn.  The
 * buffer is zeroed first, so a filler can only ever add. */
void ros_audio_add(ros_audio_fill *fn);
void ros_audio_remove(ros_audio_fill *fn);
int ros_audio_rate(void);
/* The speaker (Sound_Speaker, *Speaker): when it is off, everything the
 * chain makes is silenced after it, as RISC OS's speaker mixer channel
 * muted. A host's own sink (BBC BASIC V for Mac's) asks ros_audio_muted. */
void ros_audio_mute(int on);
int ros_audio_muted(void);
/* What the chain has made, fill by fill, since the start: frames, those
 * with a sample not zero, those at full scale (clipped, as like as not),
 * and the largest magnitude. A test can see that sound is made without
 * hearing it (the agent's "sound") */
struct ros_audio_meter {
    uint64_t frames, loud, full;
    uint32_t peak;
};
void ros_audio_meter(struct ros_audio_meter *m);
void ros_audio_sine(void);                 /* rosgd.sine: the pipe's proof */

/* ---- display ------------------------------------------------------------
 *
 * Screen memory is a DRM dumb buffer, mapped into the arena at
 * ROS_SCREEN_BASE, holding pixels in the RISC OS mode's own format (1 to 32
 * bits per pixel). It is described to the host by a screen block in the
 * buffer's last 4 KB (screen.h). The host's Metal window reads the buffer
 * straight out of guest RAM, so drawing is just writing memory and there is
 * nothing to flush. The pointer is DRM's cursor plane, drawn by the host.
 *
 * Any other display (VMware's, a PC's) is converted instead. Screen memory
 * is shared memory with the same block, and the platform's own thread turns
 * it into the display's XRGB8888, scaled and centred, pointer included, and
 * flushes it (platform/display_drm.c). Callers see no difference.
 */

/* One size and frame rate the display offers, as a monitor description
 * lists a mode: what OS_ScreenMode 2 enumerates (runtime/vdu/modes.c),
 * the DRM connector's modes, deduplicated. */
#define ROS_DISPLAY_MODES 32u
struct ros_display_mode {
    uint32_t width, height;     /* pixels */
    uint32_t rate;              /* Hz */
};

struct ros_display {
    uint32_t width, height;     /* pixels */
    uint32_t stride;            /* bytes per row */
    uint32_t bpp;               /* 1, 2, 4, 8, 16 (RGB565) or 32 */
    uint32_t pixo;              /* 32 bpp: 1 red in the low byte (&xBGR), 0 blue */
    uint32_t base;              /* arena address of screen memory */
    uint32_t size;              /* its bytes, the screen block excluded */
    uint32_t max_width;         /* the largest mode the display offers */
    uint32_t max_height;
    uint32_t start_width;       /* the start-up mode's size, if not the display's: */
    uint32_t start_height;      /* a converted display's, scaled up to it whole */
    uint32_t start_rate;        /* the frame rate it will be shown at, Hz; 0 unknown */
    struct ros_display_mode modes[ROS_DISPLAY_MODES];   /* its list, by size */
    uint32_t nmodes;            /* 0: only the largest, max_width x max_height */
    char name[32];              /* the DRM connector, e.g. "Virtual-1" */
};

/* An XRGB8888 pixel, &00RRGGBB: 32 bpp with pixo 0, the format virtio-gpu
 * declares and the start-up mode uses. */
static inline uint32_t ros_rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return (r << 16) | (g << 8) | b;
}

/* Open the first connected display and set its preferred mode, 32 bpp with
 * blue in the low byte.  0, or -errno with the display unavailable. */
int ros_display_init(struct ros_display *d);

/* Change mode: a new buffer at ROS_SCREEN_BASE, cleared, described by a new
 * screen block, and scanned out. stride 0 is the natural one, rounded up to
 * a word. rows is how many rows of that stride the buffer must hold at
 * least (0: height), because teletext keeps a second screen after the
 * first. The result is 0, or -errno with the old mode still in place:
 * -EINVAL for a format the block cannot describe, -ERANGE for a size the
 * display lacks. */
int ros_display_set_mode(struct ros_display *d, uint32_t width, uint32_t height,
                         uint32_t bpp, uint32_t pixo, uint32_t stride, uint32_t rows);

/* Palette entries first to first + n - 1, as 0x00BBGGRR. */
void ros_display_set_palette(struct ros_display *d, uint32_t first, uint32_t n,
                             const uint32_t *bgr);

/* The visible area's top left, as an address in screen memory: hardware
 * scrolling.  0, or -EINVAL if the screen would run past the buffer. */
int ros_display_set_origin(struct ros_display *d, uint32_t addr);

/* The pointer: a 64 x 64 image of 0xAARRGGBB words with its top left at
 * (x, y), in the mode's pixels; NULL hides it.  image may be NULL with
 * visible set to move the current image.  (hot_x, hot_y) is its active
 * point in the image, given with a shape: the DRM cursor's hot spot, so a
 * host console can draw the pointer where its own mouse already is rather
 * than a VSync behind (Linux's cursor plane hotspot properties). */
enum { ROS_POINTER_HIDE, ROS_POINTER_MOVE, ROS_POINTER_SHAPE };
int ros_display_set_pointer(int how, const uint32_t *image, int32_t x, int32_t y,
                            int32_t hot_x, int32_t hot_y);

/* The external windows' table in the screen block, for the compositor
 * (rosgd.display=compositor; screen.h): NULL when there is no compositor
 * or no mode.  A mode change empties it. */
struct ros_screen_ext;
struct ros_screen_ext *ros_display_ext(void);

/* For displays that do not read guest RAM (QEMU's Cocoa window): copy the
 * screen to the host.  The Metal window needs nothing. */
void ros_display_update(const struct ros_display *d);
/* Each VSync: with rosgd.dirtyfb on the kernel command line, the screen is
 * copied to the host every tenth time (5 Hz), and each mode's format is
 * said on the console. QEMU's Cocoa window and screendump see the screen
 * this way. */
void ros_display_vsync(void);

/* An absolute pointer's position, 0-65535 across the whole display, as the
 * same across the mode: they differ when a converted display shows a mode
 * scaled and centred, with borders.  axis ROS_AXIS_X or ROS_AXIS_Y. */
int32_t ros_display_map_position(uint32_t axis, int32_t fraction);

/* An absolute pointer's position, 0-65535 across the whole display, as the
 * same across the mode: they differ when a converted display shows a mode
 * scaled and centred, with borders.  axis ROS_AXIS_X or ROS_AXIS_Y. */
int32_t ros_display_map_position(uint32_t axis, int32_t fraction);

/* ---- network ------------------------------------------------------------
 *
 * Linux is the TCP/IP stack; the kernel's own DHCP configures it (ip=dhcp).
 * The Internet module is the socket interface over it (modules/internet).
 */

/* Loopback up, and /etc/resolv.conf at the name servers the kernel's DHCP
 * learned.  Returns how many IPv4 addresses are up, or -errno. */
int ros_net_init(void);

/* ---- input --------------------------------------------------------------
 *
 * Linux's input devices (evdev: virtio-input's keyboard and tablet, or any
 * other), watched by the runtime's pump. The platform translates each event
 * into RISC OS's terms: keys and mouse buttons as RISC OS's low-level key
 * numbers (hdr/Keyboard), movement, positions as a fraction of the device's
 * range, and the wheel. It delivers each at a safe point, holding the
 * lock. What RISC OS then does with it (KeyV, PointerV) is the Input
 * module's.
 */

enum ros_input_kind {
    ROS_INPUT_KEY,              /* key: a key number; value 1 down, 0 up, 2 repeat */
    ROS_INPUT_MOVE,             /* axis: value a relative movement */
    ROS_INPUT_POSITION,         /* axis: value 0-65535 across the device's range */
    ROS_INPUT_WHEEL,            /* axis: value notches, positive away and right */
    ROS_INPUT_SYNC,             /* the end of one report from a device */
#if defined(__aarch64__)
    ROS_INPUT_POWER,            /* the machine's power button, pressed (the Apple Silicon
                                   box: rosgd-vz's polite stop, gpio-keys' KEY_POWER) */
#endif
};
enum { ROS_AXIS_X, ROS_AXIS_Y };    /* Y counts downwards, as Linux does */

struct ros_input_event {
    enum ros_input_kind kind;
    uint32_t key;
    uint32_t axis;
    int32_t value;
    uint32_t device;
};

/* Open every input device; returns how many. */
int ros_input_init(void);

/* Watch them: deliver() gets each event at a safe point.  0, or -errno. */
int ros_input_start(void (*deliver)(const struct ros_input_event *ev));

const char *ros_input_device_name(uint32_t device);

/* A Linux key code as RISC OS's low-level key number, or -1: for tests. */
int ros_input_key_number(unsigned linux_code);

/* ---- time --------------------------------------------------------------- */

/* Centiseconds since the runtime started: OS_ReadMonotonicTime. */
uint32_t ros_time_cs(void);

/* ---- power and the kernel ----------------------------------------------- */

/* The kernel command line, whether it carries a word, and the value of a
 * word=value on it (NULL if there is none).  Parameters are split as
 * Linux splits them: at spaces, except in double quotes, so a value with
 * spaces is word="a b c", its value without the quotes.  ros_cmdline_find
 * is the same search in a line of the caller's: 1 if word is there (a
 * name, or a whole name=value), with *val its value (NULL if none; not
 * NUL-terminated, *vlen long) when val is not NULL. */
const char *ros_cmdline(void);
int ros_cmdline_has(const char *word);
const char *ros_cmdline_value(const char *word);
int ros_cmdline_find(const char *line, const char *word, const char **val, size_t *vlen);

/* The end of a run: power the box off, reporting status (0 success) on
 * the console.  Hosted, the process exits with it. */
__attribute__((noreturn)) void ros_poweroff(int status);
/* A restart (OS_Reset): the box reboot()s. QEMU starts it again, or ends
 * where run/run-x86_64.sh gave it -no-reboot. Hosted, the process ends as
 * at a power off. */
__attribute__((noreturn)) void ros_restart(void);

/* The platform's name, "QEMU virt" on the HAL and else "Linux", and the
 * same as a string in the arena (runtime/hardware.c): OS_Hardware's
 * HAL_PlatformName and OS_ReadSysInfo 9's subreason 7. */
const char *ros_platform_name(void);
uint32_t ros_platform_name_addr(void);

/* What the HAL has done since it started (hal/stats.c, whose
 * struct hal_stats this is): running totals, read twice to make rates.
 * ros_hal_stats fills it and returns 0 on the HAL. On Linux, or hosted,
 * it returns -1, because there is no HAL to ask. */
#define ROS_HAL_STATS_CPUS 8
struct ros_hal_stats {
    uint64_t version, ncpu, now_ns, hz;
    uint64_t mem_pages, mem_free_pages, processes, threads;
    uint64_t syscalls, irqs, faults, signals;
    uint64_t net_rx_packets, net_rx_bytes, net_tx_packets, net_tx_bytes;
    uint64_t share_calls, share_bytes, screen_flushes, screen_bytes;
    struct {
        uint64_t idle_ns, hal_ns, spin_ns, wakes, switches, ticks, pad[2];
    } cpu[ROS_HAL_STATS_CPUS];
};
int ros_hal_stats(struct ros_hal_stats *s);

/* RISC OS started again on the Linux that is already running: *Restart.
 * Seconds, where a machine's own restart goes through its firmware. */
__attribute__((noreturn)) void ros_relaunch(void);
/* OS_Reset (runtime/sysinfo.c) calls this instead, with its R0, when it is
 * set: the self-test's, which sees a reset without having one. */
extern void (*ros_reset_hook)(uint32_t r0);

#endif
