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
 * This file is a translation into C of RISC OS Open's voice source
 * (Sources/HWSupport/Sound/Voices: WaveSynth/s.WaveSynth,
 * StringLib/s.StringLib and Percussion/s.Percussion). */

/* voices.c: RISC OS's voices, done natively. They are WaveSynth, StringLib
 * and Percussion.
 *
 * This is ported from ROOL's HWSupport/Sound/Voices. Each module's voice
 * code is ported entry for entry. These are the generators that Sound1's
 * Level1Fill calls (see sound.c). Each call is given a channel's control
 * block and its bytes in the DMA buffer. The generator fills the bytes with
 * 8-bit log samples and returns the channel's flags. The three modules
 * install their voices in the ROM's order, so the slots are RISC OS 5's.
 * Slot 1 is WaveSynth-Beep. Slots 2-5 are StringLib-Soft, -Pluck, -Steel
 * and -Hard. Slots 6-9 are Percussion-Soft, -Medium, -Snare and -Noise.
 *
 * WaveSynth is a wavetable oscillator under an envelope. The envelope is a
 * series of segments. Each segment ramps the log amplitude toward a target
 * a step at a time. Each step lasts a set number of 4-sample times (or wave
 * cycles). The envelope moves on to the next segment at a wave's zero
 * crossing. StringLib is a plucked string. It has a delay line of 128
 * samples, which is filled with full-amplitude noise at the gate. A 3-tap
 * filter smooths the line every 2 cs into the wavetable that the oscillator
 * plays. Each voice's filter has a different strength. The note ends at its
 * duration. Percussion works the same way, but the pitch is fixed at the
 * drum's and the signs of the filter's outputs are flipped at random,
 * about every 271 samples.
 *
 * The registers that each voice keeps between fills are kept where the
 * original kept them. WaveSynth keeps its registers in the SCCB, from Pitch
 * on, and holds its wave segment as an offset, not an address. The others
 * keep theirs in their instance data, which is the channel's, from Sound1.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/module.h"
#include "sound.h"

static uint32_t A(const void *p)
{
    return ros_addr(p);
}

static uint32_t ror(uint32_t x, unsigned n)
{
    return n ? x >> n | x << (32 - n) : x;
}

/* ---- WaveSynth ----------------------------------------------------------------
 *
 * WaveTable0 is the Beep. It has a header, which holds the first
 * descriptor for each pitch band and the release's descriptor. The
 * envelope's descriptors start at offset 64 and are two words each. One
 * wave segment is at offset 256, in VIDC's log samples, where bit 0 is the
 * sign.
 *
 * A descriptor's first word is laid out as follows.
 *   Bits 0-6: the amplitude it ramps to.
 *   Bit 7: set to ramp down.
 *   &FF in the low byte holds at full amplitude. 0 holds at none.
 *   Bit 8: the count is in wave cycles rather than 4-sample times.
 *   Bits 9-31: the count, which is a step's time in 48 us samples.
 * Its second word holds the wave segment, and the next descriptor in
 * bits 16-31. */
union wavetable {
    struct {
        char magic[4];                  /* "!WT:" */
        char name[12];
        uint32_t len;                   /* WaveLen */
        uint32_t start[8];              /* WaveStart: by pitch band */
        uint32_t end;                   /* WaveEnd: the release */
        uint32_t pad[2];
        uint32_t desc[24][2];           /* descriptors 8-31 */
        uint8_t wave[1][256];           /* segment 1 */
    } t;
    uint32_t w[128];
    uint8_t b[512];
};

static const union wavetable beep = { .t = {
    .magic = "!WT:", .name = "Beep", .len = 512,
    .start = { 8, 8, 8, 8, 8, 8, 8, 8 },       /* every pitch: 8 */
    .end = 11,
    .desc = {
        { 0x0000007F + (1 << 9), 0x00090001 },      /* 8: attack, to &7F */
        { 0x000000F0 + (31 << 9), 0x000A0001 },     /* 9: decay, down to &70 */
        { 0x00000080 + (500 << 9), 0x000C0001 },    /* 10: sustain, down to 0 */
        { 0x00000080 + (1 << 9), 0x000C0001 },      /* 11: release, down to 0 */
        { 0, 0 },                                   /* 12: dead */
    },
    .wave = { {
        0x40, 0x68, 0x80, 0x8C, 0x9A, 0xA2, 0xA8, 0xAE, 0xB6, 0xBC, 0xC0, 0xC4, 0xC6, 0xCA, 0xCC, 0xD0,
        0xD2, 0xD4, 0xD8, 0xDA, 0xDE, 0xE0, 0xE0, 0xE2, 0xE4, 0xE4, 0xE6, 0xE8, 0xE8, 0xEA, 0xEA, 0xEC,
        0xEE, 0xEE, 0xF0, 0xF0, 0xF2, 0xF2, 0xF4, 0xF4, 0xF4, 0xF6, 0xF6, 0xF8, 0xF8, 0xF8, 0xFA, 0xFA,
        0xFA, 0xFC, 0xFC, 0xFC, 0xFC, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
        0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFC, 0xFC, 0xFC, 0xFC, 0xFA,
        0xFA, 0xFA, 0xF8, 0xF8, 0xF8, 0xF6, 0xF6, 0xF4, 0xF4, 0xF4, 0xF2, 0xF2, 0xF0, 0xF0, 0xEE, 0xEE,
        0xEC, 0xEA, 0xEA, 0xE8, 0xE8, 0xE6, 0xE4, 0xE4, 0xE2, 0xE0, 0xE0, 0xDE, 0xDA, 0xD8, 0xD4, 0xD2,
        0xD0, 0xCC, 0xCA, 0xC6, 0xC4, 0xC0, 0xBC, 0xB6, 0xAE, 0xA8, 0xA2, 0x9A, 0x8C, 0x80, 0x68, 0x40,
        0x41, 0x69, 0x81, 0x8D, 0x9B, 0xA3, 0xA9, 0xAF, 0xB7, 0xBD, 0xC1, 0xC5, 0xC7, 0xCB, 0xCD, 0xD1,
        0xD3, 0xD5, 0xD9, 0xDB, 0xDF, 0xE1, 0xE1, 0xE3, 0xE5, 0xE5, 0xE7, 0xE9, 0xE9, 0xEB, 0xEB, 0xED,
        0xEF, 0xEF, 0xF1, 0xF1, 0xF3, 0xF3, 0xF5, 0xF5, 0xF5, 0xF7, 0xF7, 0xF9, 0xF9, 0xF9, 0xFB, 0xFB,
        0xFB, 0xFD, 0xFD, 0xFD, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD, 0xFD, 0xFD, 0xFD, 0xFB,
        0xFB, 0xFB, 0xF9, 0xF9, 0xF9, 0xF7, 0xF7, 0xF5, 0xF5, 0xF5, 0xF3, 0xF3, 0xF1, 0xF1, 0xEF, 0xEF,
        0xED, 0xEB, 0xEB, 0xE9, 0xE9, 0xE7, 0xE5, 0xE5, 0xE3, 0xE1, 0xE1, 0xDF, 0xDB, 0xD9, 0xD5, 0xD3,
        0xD1, 0xCD, 0xCB, 0xC7, 0xC5, 0xC1, 0xBD, 0xB7, 0xAF, 0xA9, 0xA3, 0x9B, 0x8D, 0x81, 0x69, 0x41,
    } },
} };

_Static_assert(sizeof beep == 512 && offsetof(union wavetable, t.desc) == 64 &&
               offsetof(union wavetable, t.wave) == 256, "WaveTable0's layout");

#define DESC(off)       (beep.w[(off) >> 2])    /* a descriptor's word, by byte offset */

/* PitchStartMap. This finds the pitch band of an increment from its bits
 * 6-13. From bit 14 up the value is taken as &FF. Values 0-1 are band 0,
 * 2-3 are band 1, 4-7 are band 2, and so on up to 128-255, which is band 7.
 * The result is the number of the header's word. WaveStart is words 5-12. */
static uint32_t start_word(uint32_t idx)
{
    return idx < 2 ? 5 : 5 + (31 - (uint32_t)__builtin_clz(idx));
}

/* FixDuration. A descriptor's count is given in 48 us samples. This scales
 * it to the sample period by the factor (48 << 16) / period, which is kept
 * in Timbre. */
static uint32_t fix_duration(uint32_t r6, uint32_t r3)
{
    uint32_t r0 = (r6 >> 9) * r3;
    return (r6 & 0x1FF) | (r0 >> 16) << 9;
}

/* The note's amplitude, scaled by the volume, as an attenuation. 0 is the
 * loudest and 127 is the quietest. */
static uint32_t note_atten(const struct ros_voice_fill *f, uint32_t amp)
{
    return 127 - (f->amp[(amp & 0x7F) << 1] >> 1);
}

/* Build R1. The low byte is the note's attenuation. The top byte is that
 * attenuation plus the envelope's, which is 127 less its amplitude. Twice
 * the top byte, which is R1 >> 23, comes off each log sample. */
static uint32_t amp_scale(uint32_t r1, int32_t r7)
{
    uint32_t r0 = 127 - ((uint32_t)r7 & 0x7F);
    r1 &= 0x00FFFFFF;
    r1 |= r1 << 24;
    return r1 + (r0 << 24);
}

enum { WS_FILL, WS_GATE_ON, WS_GATE_OFF };

/* The voice. Its four entries share one body. The registers are as follows.
 *   R2: the phase and increment.
 *   R3: the duration scale.
 *   R4: the buffers left.
 *   R5: the wave segment.
 *   R6: the descriptor's count, with its target and direction below it.
 *   R7: the envelope's amplitude.
 *   R8: the descriptor's offset. */
static uint8_t wavesynth(struct ros_voice_fill *f, int entry)
{
    struct ros_sccb *cb = f->cb;
    uint32_t r1 = 0, r2 = 0, r3 = 0, r5 = 0, r6 = 0, r8 = 0, d1;
    int32_t r4 = 0, r7 = 0;
    uint8_t *p = f->p;
    unsigned k;

    if (entry == WS_GATE_ON) {
        cb->timbre = (48u << 16) / f->period;
        r1 = note_atten(f, cb->amp);
        r2 = cb->pitch, r3 = cb->timbre, r4 = (int32_t)cb->duration;
        /* the first descriptor, for the pitch's band */
        uint32_t band = r2 & 0x4000 ? 0xFF : (r2 << 18) >> 24;
        r8 = beep.w[start_word(band)] << 3;
        r6 = DESC(r8), d1 = DESC(r8 + 4);
        if (!d1)
            goto finished;
        r6 = fix_duration(r6, r3);
        r5 = (d1 & 0xFFFF) << 8;
        r7 = (r6 & 0xFF) == 0xFF ? 0x7F : 0;    /* full on, unless a ramp */
        if (r4 <= 0) {
            r7 = 0;
            goto gate_off_ramp;
        }
        goto fill_buffer;
    }
    if (entry == WS_GATE_OFF) {
        /* The duration is over, and the release starts from no amplitude.
         * A note that is gated off stops at the wave's next crossing. */
        r1 = cb->flags & SCCB_ACTIVE ? cb->amp : 0;
        cb->duration = 0;
        r1 = note_atten(f, r1);
        r2 = cb->pitch, r3 = cb->timbre, r4 = (int32_t)cb->duration;
        r5 = cb->user[0], r6 = cb->user[1], r7 = (int32_t)cb->user[2];
        if (r4 <= 0)
            r7 = 0;
        goto gate_off_ramp;
    }
    r1 = note_atten(f, cb->amp);
    r2 = cb->pitch, r3 = cb->timbre, r4 = (int32_t)cb->duration;
    r5 = cb->user[0], r6 = cb->user[1], r7 = (int32_t)cb->user[2], r8 = cb->user[3];
    if (r4 == 0)
        goto gate_off_ramp;
    goto fill_buffer;

gate_off_ramp:
    /* The release, from the amplitude it has. */
    r8 = beep.t.end << 3;
    r6 = DESC(r8), d1 = DESC(r8 + 4);
    if (!d1)
        goto finished;
    r6 = fix_duration(r6, r3);
    r7 &= 0x7F;
    r5 = (d1 & 0xFFFF) << 8;

fill_buffer:
    r1 = amp_scale(r1, r7);
    for (k = 0;;) {
        if (k == 0) {
            /* Every 4 samples: the time, if it is time that is counted. */
            if (!(r6 & 0x100)) {
                r6 -= 0x200;
                if ((int32_t)r6 < 0) {
                    /* A ramp's next step. A ramp past its target, or a
                     * hold, waits for the wave's crossing to move on. */
                    uint32_t r0 = r6 & 0xFF;
                    if (r0 != 0 && r0 != 0xFF) {
                        int32_t target = (int32_t)(r0 & 0x7F), more;
                        if (r0 & 0x80)
                            more = --r7 >= target;
                        else
                            more = ++r7 <= target;
                        if (more) {
                            r6 = fix_duration(DESC(r8), r3);
                            r1 = amp_scale(r1, r7);
                        } else {
                            r7 = target;
                        }
                    }
                }
            }
            if (p >= f->end)
                break;
        }

        /* A sample: the wave's log sample less the attenuations. */
        int32_t s = (int32_t)beep.b[r5 + (r2 >> 24)] - (int32_t)(r1 >> 23);
        *p = (uint8_t)(s < 0 ? 0 : s);
        p += f->step;
        uint32_t was = r2;
        r2 += r2 << 16;
        k = (k + 1) & 3;
        if (r2 >= was)
            continue;

        /* A wave cycle is done. It is counted, if cycles are being counted.
         * A count that has run out moves the envelope on (Advance). */
        if (r6 & 0x100)
            r6 -= 0x200;
        if ((int32_t)r6 >= 0)
            continue;
        uint32_t r0 = r6 & 0xFF;
        if (r0 != 0 && r0 != 0xFF) {
            int32_t target = (int32_t)(r0 & 0x7F), more;
            if (r0 & 0x80)
                more = --r7 >= target;
            else
                more = ++r7 <= target;
            if (more) {
                r6 = fix_duration(DESC(r8), r3);
                r1 = amp_scale(r1, r7);
                continue;
            }
        }
        /* The next segment, from the goal that this one reached. */
        int32_t goal = (int32_t)(r6 & 0x7F);
        r8 = (DESC(r8 + 4) >> 16) << 3;
        r6 = DESC(r8), d1 = DESC(r8 + 4);
        if (!d1)
            goto finished;
        r6 = fix_duration(r6, r3);
        r7 = goal;
        r5 = (d1 & 0xFFFF) << 8;
        if ((r6 & 0xFF) == 0xFF)
            r7 |= 0x7F;
        r1 = amp_scale(r1, r7);
    }

    /* The buffer is full. A buffer of the duration is gone, and at the end
     * of the duration comes the release. */
    if (--r4 == 0)
        goto gate_off_ramp;
    cb->pitch = r2, cb->timbre = r3, cb->duration = (uint32_t)r4;
    cb->user[0] = r5, cb->user[1] = r6, cb->user[2] = (uint32_t)r7, cb->user[3] = r8;
    return SCCB_ACTIVE;

finished:
    /* The envelope has ended. The phase is cleared, the rest of the buffer
     * is silent, and the channel is to be flushed. */
    cb->pitch = 0, cb->timbre = r3, cb->duration = (uint32_t)r4;
    cb->user[0] = r5, cb->user[1] = r6, cb->user[2] = (uint32_t)r7, cb->user[3] = r8;
    for (; p < f->end; p += f->step)
        *p = 0;
    return SCCB_FLUSH2;
}

static uint8_t beep_fill(struct ros_voice_fill *f)
{
    return wavesynth(f, WS_FILL);
}

static uint8_t beep_update(struct ros_voice_fill *f)
{
    return wavesynth(f, f->cb->flags & SCCB_ACTIVE ? WS_FILL : WS_GATE_OFF);
}

static uint8_t beep_gate_on(struct ros_voice_fill *f)
{
    return wavesynth(f, WS_GATE_ON);
}

static uint8_t beep_gate_off(struct ros_voice_fill *f)
{
    return wavesynth(f, WS_GATE_OFF);
}

const struct ros_voice ros_voice_beep = {
    "WaveSynth-Beep", 0, beep_fill, beep_update, beep_gate_on, beep_gate_off,
};

/* ---- StringLib and Percussion: the plucked string --------------------------------
 *
 * The instance data, DataSeg's. It holds the registers between fills and
 * the noise's seed. It holds the delay line, which is sample -1, samples
 * 0-127 and sample 128 for the wrap. It also holds the wavetable made from
 * the line, in log samples. */
struct pluck {
    uint32_t flags;
    uint32_t reg[8];                    /* RegSav1-8: R1-R8 */
    uint32_t fill_pc, kill_pc, evnt_pc;
    uint32_t seed_l, seed_h;
    int32_t line[130];                  /* DLineSave, DLine, DLine256 */
    uint8_t wave[256];                  /* WaveBuff: 128 used */
};

_Static_assert(sizeof(struct pluck) == 832, "DataSeg's size");

#define RAND_SEED       0xAAAAAAAAu
#define RAND_MASK       0x1D872B41u     /* Percussion's sign flips */
#define DRUM_PITCH      0xD055          /* the drum's pitch, as a fraction of the
                                         * centisecond: &400 of 65536*4/208 */

struct pluck_kind {
    unsigned shift;                     /* the outer taps' shift (the centre's is one less) */
    int drum;                           /* Percussion's */
};

/* The increment that overflows a 16-bit accumulator once a centisecond,
 * when it is added every 4th sample. It is 65536 x 102400 x 4 / the rate
 * (in 1/1024 Hz). */
static uint32_t centi_inc(uint32_t rate)
{
    return 0xC8000000u / (rate >> 3);
}

/* The duration's buffers, as centiseconds. This is the buffers, times the
 * samples in one, times the increment, over 65536 x 4. The samples in one
 * buffer run from this channel's first byte to the end, divided by the
 * interleave. */
static uint32_t pluck_cs(const struct ros_voice_fill *f, uint32_t r4)
{
    uint32_t r3 = f->cb->duration * r4;
    r3 *= (uint32_t)(f->end - f->p);
    if (f->step == 2)
        r3 >>= 1;
    if (f->step == 4)
        r3 >>= 2;
    else if (f->step > 4)
        r3 >>= 3;
    return r3 >> 18;
}

/* The 7-bit log amplitude as a 32-bit linear value. The value is
 * ((S+16) x 2^C - 16) x 2^19, where the low byte of the log amplitude is
 * 0CCCSSSS. */
static uint32_t log_to_linear(uint32_t r0)
{
    r0 &= 0x7F;
    r0 |= r0 << 8;
    r0 &= ~0x0FF0u;
    r0 += 0x10;
    r0 = ror(r0, 12);
    r0 = (r0 & 0xFF) >= 32 ? 0 : r0 << (r0 & 0xFF);
    r0 -= 0x01000000;
    return r0 >> 1;
}

/* The filter's rate. StringLib's runs every 2 cs. That is half the
 * centisecond's increment, with the accumulator primed to overflow soon.
 * Percussion's runs at the drum's pitch, rounded. */
static uint32_t pluck_tempo(const struct pluck_kind *k, uint32_t r4)
{
    if (!k->drum)
        return (r4 >> 1) | 0xFFFF0000u;
    uint32_t r5 = DRUM_PITCH * r4;
    r5 = (r5 >> 16) + 0xFF000000u + (r5 >> 15 & 1);
    return r5 | 0x00FF0000u;
}

/* The drum's increment. It is a quarter of the filter rate, rounded, in the
 * low half, and the phase is kept in the top half. */
static uint32_t drum_pitch(uint32_t reg1, uint32_t r5)
{
    uint32_t r1 = (reg1 & 0xFFFF0000u) | (r5 << 16) >> 18;
    return r1 + (r5 >> 1 & 1);
}

/* Filt. Each sample of the delay line is smoothed with its neighbours. The
 * new value is S(y-1)/2^n + S(y) - S(y)/2^(n-1) + S(y+1)/2^n, worked out
 * from the old values. Sample 128 stands in for the wrap. The new line is
 * then looked up as log samples, at the volume, and put into the
 * wavetable. Percussion's filter also flips the signs at random. */
static void pluck_filter(struct pluck *d, const uint8_t *log, const struct pluck_kind *k)
{
    unsigned a = k->shift, b = k->shift - 1;
    int32_t *s = d->line;
    uint32_t seed = d->seed_l;
#define TAPS(x, y, z) \
    ((int32_t)((uint32_t)((x) >> a) + (uint32_t)((z) >> a) - (uint32_t)((y) >> b) + (uint32_t)(y)))

    int32_t r4 = s[0], r5 = s[1];
    s[129] = TAPS(r4, r5, s[2]);        /* sample 128, for sample 127 */
    s[0] = s[128];                      /* the old sample 127, kept */
    for (int i = 1; i < 129; i += 4) {
        int32_t r0 = r4, r1 = r5, r2 = s[i + 1], r3 = s[i + 2];
        r4 = s[i + 3], r5 = s[i + 4];
        int32_t n[4] = { TAPS(r0, r1, r2), TAPS(r1, r2, r3), TAPS(r2, r3, r4), TAPS(r3, r4, r5) };
        for (int j = 0; j < 4; j++) {
            if (k->drum) {
                uint32_t c = seed >> 31;
                seed <<= 1;
                if (c) {
                    seed ^= RAND_MASK;
                    n[j] = (int32_t)(0u - (uint32_t)n[j]);
                }
            }
            s[i + j] = n[j];
            d->wave[i - 1 + j] = log[(uint32_t)n[j] >> 19];
        }
    }
    if (k->drum)
        d->seed_l = seed;
#undef TAPS
}

/* Fill_RND. This fills the delay line, from sample -1 on, with the
 * amplitude. The sign of each sample is taken from the bits of a 33-bit
 * shift register, which is stepped once for every 32 words. The fill runs
 * past the end of the line into the wavetable, which the filter then
 * rewrites. */
static void pluck_excite(struct pluck *d, uint32_t amp)
{
    uint32_t r0 = d->seed_l, r1 = d->seed_h;
    unsigned at = 0;
    do {
        uint32_t c = r0 & 1;
        uint32_t r2 = r0 >> 1 | r1 << 31;
        r1 = r1 + r1 + c;
        r2 ^= r0 << 12;
        r0 = r2 ^ r2 >> 20;
        for (int bit = 0; bit < 32; bit++, at++) {
            uint32_t v = r0 & 1 ? amp : 0u - amp;
            r0 = ror(r0, 1);
            if (at < 130)
                d->line[at] = (int32_t)v;
            else
                memcpy(&d->wave[(at - 130) * 4], &v, 4);
        }
    } while (at < 130);
    d->seed_l = r0;
    d->seed_h = r1;
}

static int overflows(uint32_t a, uint32_t b, uint32_t sum)
{
    return (int32_t)((a ^ sum) & (b ^ sum)) < 0;
}

/* Make the rest of the buffer silent because the duration is over. The
 * channel is to be flushed (Fill_Zero, GateOff). */
static uint8_t pluck_zero(struct ros_voice_fill *f, uint8_t *p)
{
    f->cb->duration = 0;
    for (; p < f->end; p += f->step)
        *p = 0;
    return SCCB_FLUSH2;
}

/* Fill_Wave. This runs the oscillator through the wavetable, 4 samples at a
 * time. Each centisecond takes one off the duration (R3). At the filter's
 * rate, which is when R5's accumulator overflows, it makes the next filter
 * pass. */
static uint8_t pluck_wave(struct ros_voice_fill *f, struct pluck *d, const struct pluck_kind *k)
{
    uint32_t r1 = d->reg[0], r3 = d->reg[2], r4 = d->reg[3], r5 = d->reg[4];
    uint8_t *p = f->p;
    do {
        uint32_t was = r4;
        r4 += r4 << 16;
        if (overflows(was, was << 16, r4)) {
            if ((int32_t)--r3 < 0) {
                d->reg[0] = r1, d->reg[2] = r3, d->reg[3] = r4, d->reg[4] = r5;
                return pluck_zero(f, p);
            }
        } else {
            was = r5;
            r5 += r5 << 16;
            if (overflows(was, was << 16, r5)) {
                d->reg[0] = r1, d->reg[2] = r3, d->reg[3] = r4, d->reg[4] = r5;
                pluck_filter(d, f->log, k);
            }
        }
        for (int i = 0; i < 4; i++) {
            r1 += r1 << 16;
            *p = d->wave[r1 >> 25];
            p += f->step;
        }
    } while (p < f->end);
    d->reg[0] = r1, d->reg[2] = r3, d->reg[3] = r4, d->reg[4] = r5;
    return SCCB_ACTIVE;
}

/* GateOn. The duration is set in centiseconds, up to 4095. The amplitude is
 * made linear. The increment goes in the low half and the phase in the top
 * half is kept. The noise's seed is the same each time, so each pluck is
 * the same pluck. The line is excited and filtered, and the first buffer
 * is silent. */
static uint8_t pluck_gate_on(struct ros_voice_fill *f, const struct pluck_kind *k)
{
    struct ros_sccb *cb = f->cb;
    struct pluck *d = f->inst;
    uint32_t r4 = centi_inc(f->rate);
    uint32_t r3 = pluck_cs(f, r4) & 0xFFF;
    uint32_t r5 = pluck_tempo(k, r4);
    uint32_t r2 = log_to_linear(cb->amp);
    uint32_t r1 = k->drum ? drum_pitch(d->reg[0], r5)
                          : (cb->pitch & 0xFFFF) | (d->reg[0] & 0xFFFF0000u);
    cb->user[2] = A(&d->line[1]);
    cb->user[3] = A(d->wave);
    d->seed_l = d->seed_h = RAND_SEED;
    d->reg[0] = r1, d->reg[1] = r2, d->reg[2] = r3, d->reg[3] = r4, d->reg[4] = r5;
    d->reg[5] = A(&d->line[1]), d->reg[6] = A(d->wave), d->reg[7] = A(f->log);

    pluck_excite(d, r2);
    pluck_filter(d, f->log, k);
    for (uint8_t *p = f->p; p < f->end; p += f->step)
        *p = 0;
    return SCCB_ACTIVE;
}

/* UpdateFill. This applies what the note changed, which can be the
 * duration, the amplitude or the pitch. A 0 keeps the old value. Then it
 * carries on filling. */
static uint8_t pluck_update(struct ros_voice_fill *f, const struct pluck_kind *k)
{
    struct ros_sccb *cb = f->cb;
    struct pluck *d = f->inst;
    uint32_t r4 = centi_inc(f->rate);
    uint32_t r0 = cb->amp | (uint32_t)cb->voice << 8 | (uint32_t)cb->instance << 16 |
                  (uint32_t)cb->flags << 24;
    uint32_t r1 = cb->pitch, r2, r3 = pluck_cs(f, r4), r5;
    cb->user[2] = A(&d->line[1]);
    cb->user[3] = A(d->wave);
    if (!r3) {
        r3 = d->reg[2], r4 = d->reg[3], r5 = d->reg[4];
    } else {
        r3 &= 0xFFF;
        r5 = pluck_tempo(k, r4);
    }
    r2 = r0 ? log_to_linear(r0) : d->reg[1];
    if (!r1)
        r1 = d->reg[0];
    else
        r1 = k->drum ? drum_pitch(d->reg[0], r5) : (r1 & 0xFFFF) | (d->reg[0] & 0xFFFF0000u);
    d->reg[0] = r1, d->reg[1] = r2, d->reg[2] = r3, d->reg[3] = r4, d->reg[4] = r5;
    return pluck_wave(f, d, k);
}

#define PLUCK_VOICE(id, name, sh, dr)                                                \
    static const struct pluck_kind id##_kind = { sh, dr };                           \
    static uint8_t id##_fill(struct ros_voice_fill *f)                               \
    {                                                                                \
        return pluck_wave(f, f->inst, &id##_kind);                                   \
    }                                                                                \
    static uint8_t id##_update(struct ros_voice_fill *f)                             \
    {                                                                                \
        return pluck_update(f, &id##_kind);                                          \
    }                                                                                \
    static uint8_t id##_gate_on(struct ros_voice_fill *f)                            \
    {                                                                                \
        return pluck_gate_on(f, &id##_kind);                                         \
    }                                                                                \
    static uint8_t id##_gate_off(struct ros_voice_fill *f)                           \
    {                                                                                \
        return pluck_zero(f, f->p);                                                  \
    }                                                                                \
    static const struct ros_voice id = {                                             \
        name, sizeof(struct pluck), id##_fill, id##_update, id##_gate_on, id##_gate_off, \
    };

/* StringLib's filters are Filt1 to Filt4. Their outer taps run from a
 * quarter to a 32nd. */
PLUCK_VOICE(string_soft, "StringLib-Soft", 2, 0)
PLUCK_VOICE(string_pluck, "StringLib-Pluck", 3, 0)
PLUCK_VOICE(string_steel, "StringLib-Steel", 4, 0)
PLUCK_VOICE(string_hard, "StringLib-Hard", 5, 0)

/* Percussion's outer taps run from an eighth to a 64th. */
PLUCK_VOICE(drum_soft, "Percussion-Soft", 3, 1)
PLUCK_VOICE(drum_medium, "Percussion-Medium", 4, 1)
PLUCK_VOICE(drum_snare, "Percussion-Snare", 5, 1)
PLUCK_VOICE(drum_noise, "Percussion-Noise", 6, 1)

/* ---- the modules ------------------------------------------------------------------
 *
 * Each module installs its voices in the first free slots at init, as its
 * Sound_InstallVoice 1,0 calls do. It removes them at its end. */

struct voice_set {
    const struct ros_voice *voice[4];
    uint32_t slot[4];
};

static struct voice_set wavesynth_set = { .voice = { &ros_voice_beep } };
static struct voice_set stringlib_set = { .voice = { &string_soft, &string_pluck, &string_steel, &string_hard } };
static struct voice_set percussion_set = { .voice = { &drum_soft, &drum_medium, &drum_snare, &drum_noise } };

static struct voice_set *set_of(struct ros_module *m)
{
    return m == &wavesynth_module ? &wavesynth_set
         : m == &stringlib_module ? &stringlib_set : &percussion_set;
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    struct voice_set *vs = set_of(m);
    for (int i = 0; i < 4 && vs->voice[i]; i++)
        vs->slot[i] = ros_sound_voice_install(vs->voice[i]);
    return NULL;
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    struct voice_set *vs = set_of(m);
    for (int i = 0; i < 4; i++) {
        ros_sound_voice_remove(vs->slot[i]);
        vs->slot[i] = 0;
    }
    return NULL;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    (void)s;
}

struct ros_module wavesynth_module = {
    .title = "WaveSynth",
    .help = "WaveSynth\t1.25 (01-Oct-26) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
};

struct ros_module stringlib_module = {
    .title = "StringLib",
    .help = "StringLib\t1.20 (01-Oct-26) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
};

struct ros_module percussion_module = {
    .title = "Percussion",
    .help = "Percussion\t1.19 (01-Oct-26) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
};
