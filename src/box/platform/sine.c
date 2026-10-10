/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* sine.c -- the proof of the sound pipe: a fill callback that is one
 * plain tone, started by rosgd.sine on the kernel command line and run
 * for a few seconds at startup. If the tone is heard on the host's
 * speakers, the pipe works. If there is silence, it does not. The test is
 * all this file is for.
 *
 * The tone is 440 Hz at the system rate, a little under half-scale, in
 * both channels. The phase is a 32-bit fraction advanced by the
 * frequency's 16.16 step per frame, which is the same arithmetic that
 * SharedSound's fill will use, exercised here first.
 */
#include <stdint.h>
#include <unistd.h>

#include "rosgd/platform.h"

static int32_t sine_q15(uint32_t phase);

#define HZ          440
#define SECONDS     4

static uint32_t phase;                /* 0..&FFFFFFFF, a cycle */
static uint32_t step;                 /* 16.16: Hz / rate */
static long frames_left;

static void tone(int16_t *out, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        int32_t v = 12000 * sine_q15(phase);
        phase += step;
        out[i * 2] = (int16_t)v;
        out[i * 2 + 1] = (int16_t)v;
    }
    if (frames_left > 0) {
        frames_left -= frames;
        if (frames_left <= 0)
            ros_audio_start(NULL);           /* back to silence */
    }
}

/* sin(2*pi*x) for x the whole fraction: a quarter-wave table of 256
 * entries, linear between them. 12 bits is plenty for an ear. */
/* a quarter wave, sin(i*pi/1024) * 32767, linear between entries */
static const int16_t quarter[257] = {
    0, 101, 201, 302, 402, 503, 603, 704, 804, 905,    1005, 1106, 1206, 1307, 1407, 1507, 1608, 1708, 1809, 1909,    2009, 2110, 2210, 2310, 2410, 2511, 2611, 2711, 2811, 2911,    3012, 3112, 3212, 3312, 3412, 3512, 3612, 3712, 3811, 3911,    4011, 4111, 4210, 4310, 4410, 4509, 4609, 4708, 4808, 4907,    5007, 5106, 5205, 5305, 5404, 5503, 5602, 5701, 5800, 5899,    5998, 6096, 6195, 6294, 6393, 6491, 6590, 6688, 6786, 6885,    6983, 7081, 7179, 7277, 7375, 7473, 7571, 7669, 7767, 7864,    7962, 8059, 8157, 8254, 8351, 8448, 8545, 8642, 8739, 8836,    8933, 9030, 9126, 9223, 9319, 9416, 9512, 9608, 9704, 9800,    9896, 9992, 10087, 10183, 10278, 10374, 10469, 10564, 10659, 10754,    10849, 10944, 11039, 11133, 11228, 11322, 11417, 11511, 11605, 11699,    11793, 11886, 11980, 12074, 12167, 12260, 12353, 12446, 12539, 12632,    12725, 12817, 12910, 13002, 13094, 13187, 13279, 13370, 13462, 13554,    13645, 13736, 13828, 13919, 14010, 14101, 14191, 14282, 14372, 14462,    14553, 14643, 14732, 14822, 14912, 15001, 15090, 15180, 15269, 15358,    15446, 15535, 15623, 15712, 15800, 15888, 15976, 16063, 16151, 16238,    16325, 16413, 16499, 16586, 16673, 16759, 16846, 16932, 17018, 17104,    17189, 17275, 17360, 17445, 17530, 17615, 17700, 17784, 17869, 17953,    18037, 18121, 18204, 18288, 18371, 18454, 18537, 18620, 18703, 18785,    18868, 18950, 19032, 19113, 19195, 19276, 19357, 19438, 19519, 19600,    19680, 19761, 19841, 19921, 20000, 20080, 20159, 20238, 20317, 20396,    20475, 20553, 20631, 20709, 20787, 20865, 20942, 21019, 21096, 21173,    21250, 21326, 21403, 21479, 21554, 21630, 21705, 21781, 21856, 21930,    22005, 22079, 22154, 22227, 22301, 22375, 22448, 22521, 22594, 22667,    22739, 22812, 22884, 22956, 23027, 23099, 23170
};

static int32_t sine_q15(uint32_t phase)
{
    uint32_t q = phase >> 24;                  /* 0..255: the quadrant */
    uint32_t x = (phase >> 16) & 0xFF;         /* 0..255 within it */
    int32_t a = quarter[x], b = quarter[x + 1];
    int32_t v = a + ((b - a) * (int32_t)(phase & 0xFFFF) >> 16);
    switch (q) {
    case 0: return v;
    case 1: return 32767 - v;                  /* mirrored down */
    case 2: return -v;
    default: return -(32767 - v);
    }
}

void ros_audio_sine(void)
{
    step = (uint32_t)((uint64_t)HZ * 65536 / ros_audio_rate());
    frames_left = (long)ros_audio_rate() * SECONDS;
    phase = 0;
    ros_audio_start(tone);
}
