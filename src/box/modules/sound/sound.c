/* Copyright 1996 Acorn Computers Ltd
 * Copyright 2010 Castle Technology Ltd
 * Copyright 2012 Castle Technology Ltd
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
 * This file is a translation into C of RISC OS Open's sound source
 * (Sources/HWSupport/Sound: Sound1/s.Sound1, Sound0HAL/s.Sound0 and
 * Sound0HAL/s.Sound0ARM). */

/* sound.c: the old 8-bit voice system, done natively. This does Sound1's
 * job, with Sound0's job below it.
 *
 * On RISC OS 5 the sound SWIs are split between two chunks. Sound0 owns
 * &40140 (Configure, Enable, Stereo). That is the SoundDMA's chunk. Sound1
 * owns &40180 (Volume, InstallVoice, AttachVoice, Control). That is the
 * SoundChannels' chunk. Here there are two modules, SoundDMA and Sound1,
 * and they share one workspace, which Sound1 builds. The code is ported
 * from ROOL's HWSupport/Sound. It takes Sound1's channel model (s/Sound1)
 * and the conversion that Sound0 makes of what the channels fill
 * (s/Sound0ARM). The result is mixed into the platform's 16-bit stereo
 * stream under SharedSound's handlers. It is one filler in the audio chain
 * (platform.h).
 *
 * The model is Sound1's. There are up to 8 channels. They are interleaved
 * in a DMA buffer of 8-bit log samples. Bit 0 is the sign, there are 7 bits
 * of magnitude, and there are 16 steps to an octave. Each channel has a
 * control block (the SCCB, from Hdr:Sound) and a voice. A voice is a
 * generator with Fill, Update, GateOn and GateOff entries. Level1Fill
 * calls one of these entries for each buffer, as the block's flags say.
 * The entry fills the channel's bytes and returns its flags. The voices
 * are RISC OS's own, ported in voices.c (WaveSynth, StringLib,
 * Percussion). They are installed as their modules install them.
 * Sound_Control's notes set the block's amplitude, pitch increment and
 * duration (in buffers) and the gate flags. The voice does the rest.
 *
 * Below that are Sound0's defaults. The channel sample period is 45 us
 * (SCRate, 22050 Hz). There are 224 samples a channel in a buffer
 * (SCBufferLen). Each buffer is converted. The mu-law bytes become linear
 * values, each channel is weighted by its stereo position, and the sum is
 * scaled by the channel count. The result is then oversampled 2x by linear
 * interpolation to the system's 44100 Hz. Sound0 does this for a period of
 * 42 us or more. *SoundGain's boost is 0 to 21 dB in 3 dB steps. It is
 * applied in the conversion, as Sound0 compiles it into its conversion
 * code. *Speaker is the platform's mute, which stands for Sound0's speaker
 * mixer channel.
 *
 * The music itself is not here. The queue (modules/soundsched) calls
 * ros_sound_note as beats come due. BASIC's SOUND statement compiles to
 * those calls, and so does Maestro's playback.
 */
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cmos.h"
#include "rosgd/cpu.h"
#include "rosgd/keyboard.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "sound.h"
#include "soundsched.h"

/* ---- constants, from the ObjAsm source -------------------------------------- */

#define CHANNELS_MAX    8               /* SoundPhysChannels */
#define VOICES_MAX      32              /* MaxNVoices */
#define LOG_SIZE        8192            /* the linear to log table */
#define AMP_SIZE        256             /* the log amplitude table */
#define INST_MAX        832             /* a voice's instance data: StringLib's */

#define RATE            44100           /* the system's: SharedSound's */
#define FRAMES_PER_CS   (RATE / 100)    /* centiseconds of one fill */
#define L1_RATE         22050           /* s/Sound0: SCRate, the default */
#define L1_PERIOD       (1000000 / L1_RATE)  /* SCPeriod, 45 us */
#define BUF_DEFAULT     224             /* SCBufferLen: samples a channel */
#define BUF_MAX         512             /* SoundDMABufferSize (4K) of 8 channels,
                                         * or of the oversampled linear data */

#define SOUND_CMOS      0x94            /* SoundCMOS: speaker, loudness, voice */
#define DEF_MASTER_PITCH (0xAAB0 - 0x4000)
#define BBC_PITCH_INC16 (65536 * 4096 / 48)  /* shifted left by 16 for accuracy */
#define BBC_PITCH_BASE  (0x4000 - 53 * 4096 / 48)  /* offset from middle C */
#define REFERENCE_FREQ  0x82D01134u     /* middle C, less 4 octaves, shifted left by 27 */
#define AUTO            0x4F545541u     /* "AUTO", Sound_Tuning's */

/* The sound errors from Hdr:NewErrors, with Sound1's messages. */
#define ERR_BAD_CHANNEL 0x20001
#define ERR_BAD_VOICE   0x20005
#define ERR_ILLEGAL     0x20008

/* ---- the workspace ------------------------------------------------------------ */

struct ws {
    uint32_t private_word_copy;

    /* Sound0's configuration */
    int32_t channels;                   /* interleaved: 1, 2, 4 or 8 */
    int32_t buflen;                     /* samples a channel, a buffer */
    int32_t enabled;                    /* Sound_Enable's: 1 off, 2 on */
    int32_t changed;                    /* Level0 was reconfigured: flush every channel */
    uint8_t images[CHANNELS_MAX];       /* stereo positions, &80 the centre */
    uint8_t gain;                       /* *SoundGain's, 0-7: +3 dB each */

    /* Sound1's: SoundLevel1Base's, and the module's own */
    uint8_t max_amp;                    /* SoundLevel1MaxAmp: Sound_Volume's */
    uint32_t master_pitch;              /* SoundLevel1MasterPitch */
    int32_t autotune;                   /* AutoTuneFlag */
    uint32_t per5cs;                    /* BuffersPer5cs, 20.12 */

    struct {
        uint32_t svcb;                  /* the generator, 0 none */
        const struct ros_voice *gen;    /* its code, ours */
        char local[32];                 /* the local name, "" none */
    } voice[VOICES_MAX + 1];            /* [1..32] */

    struct ros_sccb ch[CHANNELS_MAX];   /* channels 1-8 */
    uint8_t ch_pad[4];                  /* for a word read at offset 254 */
    uint8_t inst[CHANNELS_MAX][INST_MAX];

    uint8_t log[LOG_SIZE];              /* SoundLevel1LogTable */
    uint8_t amp[AMP_SIZE];              /* SoundLevel1AmpTable */

    uint8_t dma[2][CHANNELS_MAX * BUF_MAX];  /* the two buffers, filled in turn */
    int32_t dma_at;
    int16_t ring[2 * BUF_MAX][2];       /* the last buffer, converted and
                                         * oversampled, as the fills take it */
    uint32_t ring_at, ring_len;
    int16_t last[2];                    /* the frame before: oversampling's */

    char empty[4];                      /* for R0 when there is no name.
                                         * R0 must never be 0, because BASIC
                                         * would copy that as a string at
                                         * page 0 */
    char null_voice[12];                /* RemoveVoice's for an empty slot */
};

/* ---- the modules -----------------------------------------------------------------
 *
 * There are two modules, as in RISC OS 5. Sound1 owns the voices' chunk,
 * &40180. The workspace and the fill belong to Sound1. The &40140 SWIs
 * (Configure, Enable, Stereo) belong to the SoundDMA. A module's chunk is
 * 64 SWIs, so the two ranges cannot be one module's. */

static struct ws *sound1_ws;

/* The notes handed over, for ros_sound_state (the harness's view). */
static uint32_t notes_in, notes_on;

/* Set when the fill is off the audio chain, for the self-test. */
static _Atomic int held;

/* The channels are touched from the audio thread's fill and from SWIs on
 * task threads. On RISC OS the SWIs run with IRQs off and the fill is the
 * IRQ. One spinlock covers this. It is held for a note and for a buffer's
 * fill. */
static _Atomic int slock;

static void s_lock(void)
{
    while (atomic_exchange_explicit(&slock, 1, memory_order_acquire))
        while (atomic_load_explicit(&slock, memory_order_relaxed))
            ;
}

static void s_unlock(void)
{
    atomic_store_explicit(&slock, 0, memory_order_release);
}

static uint32_t A(const void *p)
{
    return ros_addr(p);
}

static struct ws *workspace(struct ros_module *m)
{
    (void)m;
    return sound1_ws;
}

static uint32_t ror(uint32_t x, unsigned n)
{
    return n ? x >> n | x << (32 - n) : x;
}

static uint32_t lsr(uint32_t x, uint32_t n)  /* ARM's LSR by register */
{
    n &= 0xFF;
    return n >= 32 ? 0 : x >> n;
}

/* ---- the pitch: Sound1's PitchTab ---------------------------------------------
 *
 * The values are (2^(x/256)) << 30, for x from 0 to 255, plus one more
 * entry for AutoTune's interpolation. A pitch is 15 bits. The top bits
 * are the octave. The low 12 bits are 1/4096 of an octave, and only the top
 * 8 of those are significant. &4000 is middle C. Once the master pitch is
 * added, the octave gives the increment's shift. */
static const uint32_t pitch_tab[257] = {
    0x40000000, 0x402C6BEA, 0x4058F6A8, 0x4085A051, 0x40B268FA, 0x40DF50B9, 0x410C57A2, 0x41397DCC,
    0x4166C34D, 0x41942839, 0x41C1ACA8, 0x41EF50AE, 0x421D1462, 0x424AF7DA, 0x4278FB2B, 0x42A71E6D,
    0x42D561B4, 0x4303C518, 0x433248AE, 0x4360EC8D, 0x438FB0CC, 0x43BE9580, 0x43ED9AC0, 0x441CC0A4,
    0x444C0741, 0x447B6EAE, 0x44AAF702, 0x44DAA054, 0x450A6ABB, 0x453A564E, 0x456A6323, 0x459A9152,
    0x45CAE0F2, 0x45FB521B, 0x462BE4E2, 0x465C9961, 0x468D6FAE, 0x46BE67E1, 0x46EF8210, 0x4720BE55,
    0x47521CC6, 0x47839D7B, 0x47B5408C, 0x47E70611, 0x4818EE22, 0x484AF8D6, 0x487D2646, 0x48AF768A,
    0x48E1E9BA, 0x49147FEE, 0x4947393F, 0x497A15C5, 0x49AD1598, 0x49E038D1, 0x4A137F88, 0x4A46E9D7,
    0x4A7A77D5, 0x4AAE299C, 0x4AE1FF44, 0x4B15F8E6, 0x4B4A169C, 0x4B7E587E, 0x4BB2BEA5, 0x4BE7492B,
    0x4C1BF829, 0x4C50CBB8, 0x4C85C3F1, 0x4CBAE0EF, 0x4CF022CA, 0x4D25899C, 0x4D5B157F, 0x4D90C68C,
    0x4DC69CDD, 0x4DFC988D, 0x4E32B9B4, 0x4E69006E, 0x4E9F6CD4, 0x4ED5FF00, 0x4F0CB70D, 0x4F439514,
    0x4F7A9931, 0x4FB1C37D, 0x4FE91413, 0x50208B0E, 0x50582888, 0x508FEC9C, 0x50C7D765, 0x50FFE8FE,
    0x51382182, 0x5170810B, 0x51A907B5, 0x51E1B59A, 0x521A8AD7, 0x52538787, 0x528CABC4, 0x52C5F7AA,
    0x52FF6B55, 0x533906E1, 0x5372CA68, 0x53ACB608, 0x53E6C9DB, 0x542105FD, 0x545B6A8C, 0x5495F7A1,
    0x54D0AD5B, 0x550B8BD4, 0x5546932A, 0x5581C378, 0x55BD1CDB, 0x55F89F70, 0x56344B53, 0x567020A0,
    0x56AC1F75, 0x56E847EF, 0x57249A2A, 0x57611643, 0x579DBC57, 0x57DA8C84, 0x581786E6, 0x5854AB9C,
    0x5891FAC1, 0x58CF7475, 0x590D18D4, 0x594AE7FB, 0x5988E20A, 0x59C7071D, 0x5A055751, 0x5A43D2C7,
    0x5A82799A, 0x5AC14BEA, 0x5B0049D5, 0x5B3F7378, 0x5B7EC8F2, 0x5BBE4A62, 0x5BFDF7E6, 0x5C3DD19C,
    0x5C7DD7A4, 0x5CBE0A1C, 0x5CFE6923, 0x5D3EF4D8, 0x5D7FAD59, 0x5DC092C7, 0x5E01A540, 0x5E42E4E3,
    0x5E8451D0, 0x5EC5EC26, 0x5F07B405, 0x5F49A98C, 0x5F8BCCDC, 0x5FCE1E13, 0x60109D51, 0x60534AB7,
    0x60962665, 0x60D9307B, 0x611C6919, 0x615FD05F, 0x61A3666D, 0x61E72B65, 0x622B1F66, 0x626F4292,
    0x62B39509, 0x62F816EC, 0x633CC85B, 0x6381A978, 0x63C6BA64, 0x640BFB41, 0x64516C2E, 0x64970D4F,
    0x64DCDEC3, 0x6522E0AD, 0x6569132D, 0x65AF7666, 0x65F60A79, 0x663CCF88, 0x6683C5B4, 0x66CAED20,
    0x671245ED, 0x6759D03E, 0x67A18C35, 0x67E979F4, 0x6831999E, 0x6879EB55, 0x68C26F3B, 0x690B2574,
    0x69540E22, 0x699D2968, 0x69E67769, 0x6A2FF848, 0x6A79AC28, 0x6AC3932C, 0x6B0DAD77, 0x6B57FB2E,
    0x6BA27C72, 0x6BED3168, 0x6C381A33, 0x6C8336F7, 0x6CCE87D8, 0x6D1A0CF8, 0x6D65C67D, 0x6DB1B48A,
    0x6DFDD743, 0x6E4A2ECD, 0x6E96BB4B, 0x6EE37CE3, 0x6F3073B8, 0x6F7D9FF0, 0x6FCB01AE, 0x70189918,
    0x70666653, 0x70B46983, 0x7102A2CD, 0x71511257, 0x719FB845, 0x71EE94BD, 0x723DA7E4, 0x728CF1E0,
    0x72DC72D6, 0x732C2AEB, 0x737C1A46, 0x73CC410C, 0x741C9F63, 0x746D3571, 0x74BE035D, 0x750F094C,
    0x75604765, 0x75B1BDCD, 0x76036CAD, 0x7655542A, 0x76A7746B, 0x76F9CD97, 0x774C5FD5, 0x779F2B4C,
    0x77F23022, 0x78456E80, 0x7898E68C, 0x78EC986E, 0x7940844E, 0x7994AA53, 0x79E90AA5, 0x7A3DA56B,
    0x7A927ACE, 0x7AE78AF6, 0x7B3CD60B, 0x7B925C35, 0x7BE81D9C, 0x7C3E1A6A, 0x7C9452C7, 0x7CEAC6DB,
    0x7D41D96E, 0x7D98C9E6, 0x7DEFF6B7, 0x7E476009, 0x7E9F0607, 0x7EF6E8DB, 0x7F4F08AE, 0x7FA765AD,
    0x7FFFFFFF,
};

/* The increment a sample of a 16-bit phase, for a 15-bit pitch. This is
 * SoundShared's calculation. The master pitch is added and the octave is
 * inverted to a shift. */
static uint32_t pitch_inc(const struct ws *w, uint32_t pitch)
{
    uint32_t r = ror(pitch + w->master_pitch, 12) & ~0x00FF0000u;
    return lsr(pitch_tab[r >> 24], r ^ 0x1F);
}

/* AutoTune. This works out the tuning that makes &4000 middle C at the
 * rate (r2, in 1/1024 Hz). The reference is divided by the rate and
 * normalised to an octave count and a fraction. The fraction is found in
 * PitchTab, and its last 4 bits are interpolated. This also works out the
 * number of buffers in 5 cs, which is the unit of Sound_Control's
 * durations. */
static void autotune(struct ws *w)
{
    uint32_t r2 = (uint32_t)L1_RATE << 10, r3 = (uint32_t)w->buflen * 5;
    w->per5cs = (r2 + (r3 >> 1)) / r3;
    if (!w->autotune)
        return;

    uint32_t r1 = REFERENCE_FREQ;
    int32_t r0 = __builtin_clz(r2);
    r2 <<= r0;
    if (r2 > r1)
        r2 >>= 1, r0--;
    r3 = 0;
    for (int i = 0; i < 31; i++) {      /* a 31-bit fraction. The numerator
                                         * is shifted, not the divisor */
        uint32_t c = r1 >= r2;
        if (c)
            r1 -= r2;
        r3 = r3 + r3 + c;
        r1 <<= 1;
    }
    unsigned at = 0;
    for (unsigned step = 128; step; step >>= 1)
        if ((int32_t)pitch_tab[at + step] < (int32_t)r3)
            at += step;
    r0 = (r0 << 12) + (int32_t)(at << 4);
    uint32_t d = pitch_tab[at + 1] - pitch_tab[at];
    r0 += (int32_t)(((r3 - pitch_tab[at]) * 33) / d >> 1);  /* times 16.5, which rounds */
    if (r0 <= 0)
        r0 = 1;
    if (r0 >= 0x8000)
        r0 = 0x7FFF;
    w->master_pitch = (uint32_t)r0;
}

/* ---- the log tables (BuildLogTable) -------------------------------------------
 *
 * The linear to log table turns a 13-bit two's complement linear value into
 * a log byte. There are 16 steps to the octave, and each step is 2 in the
 * byte. Positive values run up from the start of the table and negative
 * values run down from the end. Twice the attenuation is taken off each
 * byte. The attenuation is 127 less Sound_Volume's value, so the volume
 * scales every voice that looks its samples up here. The amplitude table
 * scales a log byte in the same way. */
static void build_log_table(struct ws *w)
{
    int32_t atten = 0x7F - (w->max_amp & 0x7F);
    uint8_t *pos = w->log, *neg = w->log + LOG_SIZE;
    int32_t amp = 0;
    *pos++ = 0;                         /* 0 is special */
    for (uint32_t per = 1;; per <<= 1) {     /* a chord: 16 steps, each of them per wide */
        for (int step = 0; step < 16; step++) {
            amp += 2;
            if (amp > 0xFE)
                amp = 0xFE;
            int32_t v = amp - atten * 2;
            if (v < 0)
                v = 0;
            for (uint32_t n = per; n; n--) {
                *pos++ = (uint8_t)v;
                *--neg = (uint8_t)(v | 1);
                if (pos >= neg)
                    goto amps;
            }
        }
    }
amps:
    for (int i = 0; i < AMP_SIZE; i++) {
        int32_t v = i - atten * 2;
        w->amp[i] = (uint8_t)(v < 0 ? 0 : v);
    }
}

/* ---- Sound0: a buffer's log bytes to 16-bit stereo ------------------------------
 *
 * The linear values of the mu-law bytes (convtable). The 7-bit magnitude
 * is in 8 chords of 16. Each chord's step is twice the last. The values run
 * from 0 to 3952, and bit 0 is the sign. */
static int16_t mulaw[256];

static void build_mulaw(void)
{
    int32_t v = 0;
    for (int k = 0; k < 128; k++) {
        mulaw[k * 2] = (int16_t)v;
        mulaw[k * 2 + 1] = (int16_t)-v;
        v += 1 << (k >> 4);
    }
}

/* A stereo position is &01-&FF, which is Sound_Stereo's -127..127 plus &80.
 * This turns it into one of the 7 positions that the conversion code is
 * compiled for (ConvImages). 1 is all left, 4 is the centre and 7 is all
 * right. */
static int image_pos(uint8_t b)
{
    uint32_t v = b < 0xE0 ? b + 0x10u : b;
    v >>= 5;
    return v ? (int)v : 1;
}

/* *SoundGain's boosts. Each is 10^(3g/20) times 1024. These are the scale
 * factors that Sound0's tables carry for +0 to +21 dB. */
static const int32_t gain_x1024[8] = { 1024, 1446, 2043, 2886, 4077, 5759, 8135, 11491 };

static int16_t gained(int32_t v, int32_t g)
{
    if (g != 1024)
        v = (int32_t)(((int64_t)v * g) >> 10);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* The conversion of one buffer. Each channel's linear value is split into 6
 * parts between the two sides, as its position puts it (6:0, 5:1, 4:2,
 * 3:3 and so on). The sum is multiplied by 11/64 for 8 channels, by 11/8 for
 * 1 channel and by 11/16 for 2. Then N channels, each at full scale and hard
 * over, just reach full scale (with *SoundGain 0, the default). */
static void level0_convert(const struct ws *w, const uint8_t *buf, int16_t (*out)[2])
{
    unsigned n = (unsigned)w->channels;
    int32_t g = gain_x1024[w->gain & 7];
    unsigned shift = 3 + (n >= 8 ? 3 : n >= 4 ? 2 : n >= 2 ? 1 : 0);
    int32_t wl[CHANNELS_MAX], wr[CHANNELS_MAX];
    for (unsigned c = 0; c < n; c++) {
        int p = image_pos(w->images[c]);
        wl[c] = 7 - p;
        wr[c] = p - 1;
    }
    for (int32_t i = 0; i < w->buflen; i++, buf += n) {
        int32_t l = 0, r = 0;
        for (unsigned c = 0; c < n; c++) {
            int32_t v = mulaw[buf[c]];
            l += wl[c] * v;
            r += wr[c] * v;
        }
        out[i][0] = gained((l * 11) >> shift, g);
        out[i][1] = gained((r * 11) >> shift, g);
    }
}

/* ---- Sound1: the channels' fill (Level1Fill) ------------------------------------
 *
 * Each channel's voice is entered as its flags say, with the bytes it
 * fills. GateOff is tried before GateOn, GateOn before Update and Update
 * before Fill. A quiet channel with a flush pending has its bytes cleared,
 * once a buffer, as the count says. The count is two because the DMA has two
 * buffers. A gate that the note left pending through the call is patched
 * in. A GateOff with a GateOn behind it calls GateOn next time. */
static void level1_fill(struct ws *w, uint8_t *buf)
{
    unsigned n = (unsigned)w->channels;
    int changed = w->changed;
    w->changed = 0;
    for (unsigned c = 0; c < n; c++) {
        struct ros_sccb *cb = &w->ch[c];
        uint8_t f = cb->flags;
        if (changed)
            f = (uint8_t)((f | SCCB_FLUSH2) & ~SCCB_FLUSH1);
        uint8_t *p = buf + c, *end = buf + n * (unsigned)w->buflen;

        if (f >= SCCB_ACTIVE) {
            const struct ros_voice *gen = cb->voice < VOICES_MAX ? w->voice[cb->voice].gen : NULL;
            if (gen) {
                struct ros_voice_fill vf = {
                    .cb = cb, .channel = c, .p = p, .end = end, .step = n,
                    .period = L1_PERIOD, .rate = (uint32_t)L1_RATE << 10,
                    .inst = w->inst[c], .log = w->log, .amp = w->amp,
                };
                uint8_t (*entry)(struct ros_voice_fill *) = gen->fill;
                if (f & SCCB_UPDATE)
                    entry = gen->update;
                if (f & SCCB_GATE_ON)
                    entry = gen->gate_on;
                if (f & SCCB_GATE_OFF)
                    entry = gen->gate_off;
                uint8_t r0 = entry(&vf);
                uint8_t r1 = cb->flags;
                if (r1 & SCCB_GATE_OFF)
                    r1 &= (uint8_t)~SCCB_GATE_OFF;
                else if (r1 & SCCB_GATE_ON)
                    r1 &= (uint8_t)~(SCCB_GATE_ON | SCCB_FLUSH);
                else
                    r1 = 0;
                cb->flags = r1 | r0;
                continue;
            }
            f = SCCB_FLUSH2;            /* no voice: flush once, then quiet */
        } else if (!(f & SCCB_FLUSH)) {
            continue;
        }
        cb->flags = (uint8_t)(f - SCCB_FLUSH1);
        for (; p < end; p += n)
            *p = 0;
    }
}

/* The next buffer. It is filled, converted and oversampled into the ring.
 * Each frame is preceded by its mean with the frame before it
 * (DoFunc_Oversample, which uses SHADD16's halving add). */
static void next_buffer(struct ws *w)
{
    uint8_t *buf = w->dma[w->dma_at];
    w->dma_at ^= 1;
    level1_fill(w, buf);

    int16_t lin[BUF_MAX][2];
    level0_convert(w, buf, lin);
    for (int32_t i = 0; i < w->buflen; i++) {
        for (int s = 0; s < 2; s++) {
            w->ring[i * 2][s] = (int16_t)((w->last[s] + lin[i][s]) >> 1);
            w->ring[i * 2 + 1][s] = lin[i][s];
            w->last[s] = lin[i][s];
        }
    }
    w->ring_at = 0;
    w->ring_len = (uint32_t)w->buflen * 2;
}

/* ---- the note: Sound_Control's (SoundShared) -------------------------------------- */

void ros_sound_note(uint32_t channel, uint32_t r1, int32_t pitch, uint32_t duration)
{
    struct ws *w = workspace(&sound_module);
    uint32_t c = (channel & 0x0F) - 1;  /* byte 1 is the queue's flags */
    if (!w || c >= CHANNELS_MAX)
        return;
    /* *FX210 with a non-zero value suppresses every note. Immediate and
     * queued notes both come here. SoundShared reads the kernel's byte for
     * this (SoundSuppressAddr, OS_Byte 210's variable). */
    if (ros_byte_var(210))
        return;

    /* The amplitude, as Sound1 reads R1's low half (#71). &01xx is Sound1's
     * own coding. &100 and &180 gate off. &101-&17F gate on at that
     * amplitude. &181-&1FF are a smooth update, which changes the note's
     * amplitude and pitch as it sounds. &0000 and &FFxx are the BBC's
     * amplitudes: -15 (the loudest) to -1, and 0 for off. 1-&FF is a BBC
     * envelope, which Sound1 never accepted. Anything else is not a note. */
    uint32_t hi = (r1 >> 8) & 0xFF;
    if (hi == 0 || hi == 0xFF) {
        int32_t bbc = (int16_t)r1;
        if (bbc > 0)
            return;                     /* an envelope */
        r1 = bbc ? (((uint32_t)(bbc - 1) & 0x0F) << 2) ^ 0x7F : 0;
    } else if (hi != 1) {
        return;
    }

    s_lock();
    struct ros_sccb *cb = &w->ch[c];
    cb->amp = (uint8_t)(r1 & 0x7F);

    /* The pitch. Under 256 it is a BBC micro note number. Each number is a
     * quarter semitone and 53 is middle C, with 48 to the octave (#71).
     * Under &8000 it is Sound1's 15 bits. Otherwise it is the increment
     * itself. The increment goes into the low half and the phase is left
     * alone, so a smooth update keeps the wave. */
    uint32_t inc = (uint32_t)pitch;
    if (pitch < 0x8000) {
        uint32_t p = (uint32_t)pitch;
        if (pitch < 256)
            p = ((uint32_t)pitch * BBC_PITCH_INC16 >> 16) + BBC_PITCH_BASE;
        inc = pitch_inc(w, p);
    }
    cb->pitch = (cb->pitch & 0xFFFF0000u) | (inc & 0xFFFF);

    /* The duration is in 5 cs periods, and &FF is for ever. It is stored
     * as the number of buffers to fill. 0 keeps the old duration. The voice
     * counts it down. Once it was stored here and never counted, so every
     * note ended at once and Maestro's notes were clicks (#62). */
    uint32_t d = 0x7FFFFFFF;
    if (duration != 0xFF) {
        uint64_t m = (uint64_t)duration * w->per5cs;
        if (m >> 32 < 1u << 19)
            d = (uint32_t)(m >> 12);
    }
    if (d)
        cb->duration = d;

    uint8_t f = cb->flags & 0x1F;
    if (!(r1 & 0x7F))
        f |= SCCB_GATE_OFF;
    f |= r1 & 0x80 ? SCCB_UPDATE : SCCB_GATE_ON;
    cb->flags = f;
    s_unlock();

    notes_in++;
    notes_on += !(r1 & 0x80) && (r1 & 0x7F);
}

void ros_sound_state(uint32_t *in, uint32_t *on)
{
    *in = notes_in;
    *on = notes_on;
}

/* The fill. The queue's beat passes, then the channels mix in. This runs on
 * the audio thread, as SharedSound's handlers do. The queue's tick stands
 * for the DMA interrupt's dispatch. */
static void sound_fill(int16_t *out, size_t frames)
{
    struct ws *w = workspace(&sound_module);
    if (!w)
        return;

    /* The centiseconds that this fill covers. The part-centisecond is
     * carried to the next fill. A fill of 880 frames is 1.995 cs. Dropping
     * the .995 each time made the beat run at half speed (#62). */
    static size_t spare;                /* frames under one centisecond */
    spare += frames;
    unsigned cs = (unsigned)(spare / FRAMES_PER_CS);
    spare %= FRAMES_PER_CS;

    ros_soundq_tick(cs);
    if (!atomic_load(&held))
        ros_sound_mix(out, frames);
}

/* Add the channels' sound into out (see sound.h). The sound comes from the
 * ring's frames. A buffer is filled whenever the ring is empty. */
void ros_sound_mix(int16_t *out, size_t frames)
{
    struct ws *w = workspace(&sound_module);
    if (!w)
        return;
    s_lock();
    while (w->enabled == 2 && frames) {
        if (w->ring_at >= w->ring_len)
            next_buffer(w);
        while (frames && w->ring_at < w->ring_len) {
            /* Add to what the fillers before this one made, as all of
             * platform.h's fillers do. This keeps the sound of
             * SharedSound's clients (#70). */
            for (int s = 0; s < 2; s++) {
                int32_t v = out[s] + w->ring[w->ring_at][s];
                out[s] = (int16_t)(v > 32766 ? 32766 : v < -32766 ? -32766 : v);
            }
            out += 2;
            frames--;
            w->ring_at++;
        }
    }
    s_unlock();
}

void ros_sound_hold(int h)
{
    atomic_store(&held, h);
}

/* ---- the voice table ------------------------------------------------------------ */

/* The generators that the voice modules gave, by their SVCB's address. */
static struct {
    uint32_t svcb;
    const struct ros_voice *gen;
} known[16];

static const struct ros_voice *generator(uint32_t svcb)
{
    if (!svcb)
        return NULL;
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (known[i].svcb == svcb)
            return known[i].gen;
    /* The box cannot call this one, because it is ARM code. WaveSynth's
     * generator stands in for it. */
    return &ros_voice_beep;
}

/* A slot's title, from its SVCB. The offset is at SoundVoiceTitle. */
static uint32_t title_of(const struct ws *w, uint32_t slot)
{
    uint32_t b = w->voice[slot].svcb;
    return b >= 0x8000u ? b + ros_ld32(b + 28) : 0;
}

/* AttachVoice. The old voice comes off the channel. The new one goes on with
 * a fresh instance (its data zeroed) and the channel is flushed. A slot with
 * no voice in it is attached by number, so that the voice is found when one
 * is installed. This returns NULL, or the error. */
static os_error *attach(struct ws *w, uint32_t c, uint32_t v, uint32_t *old)
{
    struct ros_sccb *cb = &w->ch[c];
    *old = cb->voice;
    cb->voice = 0;
    if (v > VOICES_MAX) {
        *old = 0;
        return ros_error(ERR_BAD_VOICE, "Sound voice must be in the range 0-32");
    }
    if (!v)
        return NULL;
    cb->voice = (uint8_t)v;
    if (!w->voice[v].gen)
        return NULL;
    memset(w->inst[c], 0, sizeof w->inst[c]);
    cb->flags = SCCB_FLUSH2;
    return NULL;
}

/* CheckAttachments. Each channel attached to this slot by number gets an
 * instance of the new voice. */
static void check_attachments(struct ws *w, uint32_t slot)
{
    for (int c = CHANNELS_MAX - 1; c >= 0; c--)
        if (w->ch[c].voice == slot) {
            uint32_t old;
            w->ch[c].voice = 0;
            attach(w, (uint32_t)c, slot, &old);
        }
}

/* Copy a local name, up to the first control character. */
static void set_local(struct ws *w, uint32_t slot, const char *name)
{
    size_t n = 0;
    while (n < sizeof w->voice[slot].local - 1 && (uint8_t)name[n] >= 32)
        n++;
    memcpy(w->voice[slot].local, name, n);
    w->voice[slot].local[n] = 0;
}

static void install(struct ws *w, uint32_t slot, uint32_t svcb)
{
    if (svcb < 0x8000u)                 /* no generator (Install 0,0), so
                                         * the slot stays free */
        svcb = 0;
    w->voice[slot].svcb = svcb;
    w->voice[slot].gen = generator(svcb);
    w->voice[slot].local[0] = 0;
    check_attachments(w, slot);
}

static void remove_voice(struct ws *w, uint32_t slot)
{
    /* CheckRemovals. Detach the voice from each channel that has it, but
     * keep the voice number. */
    for (int c = CHANNELS_MAX - 1; c >= 0; c--)
        if (w->ch[c].voice == slot) {
            uint32_t old;
            attach(w, (uint32_t)c, 0, &old);
            w->ch[c].voice = (uint8_t)slot;
        }
    w->voice[slot].svcb = 0;
    w->voice[slot].gen = NULL;
    w->voice[slot].local[0] = 0;
}

static uint32_t first_free(const struct ws *w)
{
    for (uint32_t slot = 1; slot <= VOICES_MAX; slot++)
        if (!w->voice[slot].svcb)
            return slot;
    return 0;
}

/* A voice module's install. The SVCB is built in the RMA. It holds the
 * entries (code that the box keeps as C), the title's offset and the
 * instance size. The voice goes in the first free slot, with its title as
 * its local name. The modules' Messages give the same name. */
uint32_t ros_sound_voice_install(const struct ros_voice *v)
{
    struct ws *w = workspace(&sound_module);
    if (!w)
        return 0;
    size_t len = strlen(v->title) + 1;
    void *block;
    if (xos_module_claim((uint32_t)(40 + len), &block))
        return 0;
    uint32_t *svcb = block;
    memset(svcb, 0, 40);
    svcb[7] = 40;                       /* SoundVoiceTitle */
    svcb[8] = v->dsize;                 /* SoundVoiceDSize */
    memcpy((char *)block + 40, v->title, len);

    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!known[i].svcb) {
            known[i].svcb = A(block);
            known[i].gen = v;
            break;
        }

    s_lock();
    uint32_t slot = first_free(w);
    if (slot) {
        install(w, slot, A(block));
        set_local(w, slot, v->title);
        svcb[9] = slot;                 /* SoundVoiceIndex */
    }
    s_unlock();
    return slot;
}

void ros_sound_voice_remove(uint32_t slot)
{
    struct ws *w = workspace(&sound_module);
    if (!w || slot == 0 || slot > VOICES_MAX || !w->voice[slot].svcb)
        return;
    s_lock();
    remove_voice(w, slot);
    s_unlock();
}

/* ---- the SWIs ---------------------------------------------------------------- */

static struct ws *ws_or_fail(struct ros_cpu *s)
{
    struct ws *w = workspace(&sound_module);
    if (!w)
        ros_swi_fail(s, ros_error(0x1A6, "Sound system not present"));
    return w;
}

/* Sound0's Configure. R0 is the number of channels, rounded up to 1, 2, 4
 * or 8. R1 is the samples a channel in the buffer. 0 leaves each as it is.
 * The period returned is SCPeriod's, because there is only one rate. The
 * old values come back. */
void ros_thunk_Sound_Configure(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t n = s->r[0], b = s->r[1];
    s_lock();
    uint32_t old_n = (uint32_t)w->channels, old_b = (uint32_t)w->buflen;
    if (n) {
        n = n <= 1 ? 1 : n <= 2 ? 2 : n <= 4 ? 4 : 8;
        if (n != old_n)
            w->channels = (int32_t)n, w->changed = 1;
    }
    if (b) {
        b = (b + 3) & ~3u;              /* the fills go 4 samples at a time */
        b = b < 8 ? 8 : b > BUF_MAX ? BUF_MAX : b;
        if (b != old_b) {
            w->buflen = (int32_t)b, w->changed = 1;
            autotune(w);                /* Service_SoundConfigChanging */
        }
    }
    s_unlock();
    s->r[0] = old_n;
    s->r[1] = old_b;
    s->r[2] = L1_PERIOD;
    s->r[3] = 0;
    s->r[4] = 0;
}

void ros_thunk_Sound_Enable(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t state = s->r[0];
    if (state == 1)
        w->enabled = 1;                 /* off */
    else if (state == 2)
        w->enabled = 2;                 /* on */
    s->r[0] = (uint32_t)w->enabled;
}

/* Sound0Stereo. The position is -127 (left) to 127 (right), and -128
 * reads. It is set for the channel and for each of its images, which recur
 * every channel count. The old position comes back, or -128 for a channel
 * out of range. */
void ros_thunk_Sound_Stereo(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t c = s->r[0] - 1;
    if (c >= CHANNELS_MAX) {
        s->r[1] = (uint32_t)-128;
        return;
    }
    int32_t old = (int32_t)w->images[c] - 0x80;
    uint32_t b = s->r[1] + 0x80;
    if (b != 0 && b < 0x100) {
        s_lock();
        for (uint32_t k = c; k < CHANNELS_MAX; k += (uint32_t)w->channels)
            w->images[k] = (uint8_t)b;
        s_unlock();
    }
    s->r[1] = (uint32_t)old;
}

/* Sound0Speaker. 0 reads, 1 turns the speaker off and 2 turns it on. The old
 * state comes back (1 off, 2 on). The speaker is the whole output, so the
 * platform mutes it, as Sound0 mutes its mixer's speaker channel. */
void ros_thunk_Sound_Speaker(struct ros_cpu *s)
{
    uint32_t old = ros_audio_muted() ? 1 : 2;
    if (s->r[0] == 1 || s->r[0] == 2)
        ros_audio_mute(s->r[0] == 1);
    s->r[0] = old;
}

void ros_thunk_Sound_Mode(struct ros_cpu *s)
{
    if (s->r[0] == 0) {
        s->r[0] = 0;                    /* no 16-bit direct mode */
        s->r[1] = 0;
    }
}

void ros_thunk_Sound_LinearHandler(struct ros_cpu *s)
{
    s->r[1] = 0;                        /* SharedSound owns the linear path */
    s->r[2] = 0;
}

/* The one rate that the channels run at, in 1/1024 Hz (the PRM's units). */
void ros_thunk_Sound_SampleRate(struct ros_cpu *s)
{
    if (s->r[0] == 0) {
        s->r[1] = 1;                    /* one rate */
    } else if (s->r[0] == 1) {
        s->r[1] = 1;
        s->r[2] = (uint32_t)L1_RATE << 10;
    } else if (s->r[0] == 2 || s->r[0] == 3) {
        s->r[2] = (uint32_t)L1_RATE << 10;
    }
}

/* SoundVol. The volume is taken MOD 128, and 0 reads. The log tables are
 * rebuilt. The old volume comes back. */
void ros_thunk_Sound_Volume(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t old = w->max_amp;
    uint32_t v = s->r[0] & 0x7F;
    if (v) {
        s_lock();
        w->max_amp = (uint8_t)v;
        build_log_table(w);
        s_unlock();
    }
    s->r[0] = old;
}

/* Turn a 32-bit linear value (using its top 13 bits) into log, at the
 * volume. */
void ros_thunk_Sound_SoundLog(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (w)
        s->r[0] = w->log[s->r[0] >> 19];
}

/* Scale a log byte by the volume. */
void ros_thunk_Sound_LogScale(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (w)
        s->r[0] = w->amp[s->r[0] & 0xFF];
}

/* Sound_InstallVoice. R0 of 0 reads slot R1's name into R0. 2 reads both
 * names into R2 and R3. 1 installs the generator at R2, with R3 as its local
 * name. 3 gives slot R1 a local name. Anything else is an old-style
 * generator in R0. Slot 0 is always an install, into the first free slot.
 * Maestro uses this as "SYS Install 0,0" to count the voices there are. */
void ros_thunk_Sound_InstallVoice(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t r0 = s->r[0], slot = s->r[1];
    if (slot > VOICES_MAX) {
        ros_swi_fail(s, ros_error(ERR_ILLEGAL, "Illegal voice index"));
        return;
    }
    if (slot && r0 == 3) {
        if (s->r[3] >= 0x8000u)
            set_local(w, slot, ros_ptr(s->r[3]));
        return;
    }
    if (slot && (r0 == 0 || r0 == 2)) {
        uint32_t name = title_of(w, slot);
        if (r0 == 0) {
            s->r[0] = name;
            return;
        }
        s->r[2] = name;
        s->r[3] = name && w->voice[slot].local[0] ? A(w->voice[slot].local) : name;
        return;
    }

    if (!slot) {
        slot = first_free(w);
        if (!slot) {
            s->r[1] = 0;                /* the table is full */
            return;
        }
    } else if (w->voice[slot].svcb) {
        s->r[0] = title_of(w, slot);    /* taken: return its name and fail */
        s->r[1] = 0;
        return;
    }
    uint32_t svcb = r0 == 1 ? s->r[2] : r0;
    s_lock();
    install(w, slot, svcb);
    if (r0 == 1 && s->r[3] >= 0x8000u)
        set_local(w, slot, ros_ptr(s->r[3]));
    s_unlock();
    s->r[1] = slot;
    if (r0 == 0)
        s->r[0] = A(w->empty);          /* Install 0,0: see empty */
}

void ros_thunk_Sound_RemoveVoice(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t slot = s->r[1];
    if (slot == 0 || slot > VOICES_MAX) {
        ros_swi_fail(s, ros_error(ERR_ILLEGAL, "Illegal voice index"));
        return;
    }
    if (!w->voice[slot].svcb) {
        s->r[0] = A(w->null_voice);     /* it wasn't allocated */
        return;
    }
    s->r[0] = title_of(w, slot);
    s_lock();
    remove_voice(w, slot);
    s_unlock();
}

void ros_thunk_Sound_AttachVoice(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t c = s->r[0] - 1, old;
    if (c >= CHANNELS_MAX) {
        s->r[1] = 0;
        ros_swi_fail(s, ros_error(ERR_BAD_CHANNEL, "Channel number must be in the range 1-8"));
        return;
    }
    s_lock();
    os_error *e = attach(w, c, s->r[1], &old);
    s_unlock();
    s->r[1] = old;                      /* the voice it had, 0 none */
    if (e)
        ros_swi_fail(s, e);
}

/* Attach the voice that has the given title, by number. The title must
 * match exactly. Spaces before it are skipped, and a control character or
 * space ends it. R1 is kept. */
void ros_thunk_Sound_AttachNamedVoice(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t c = s->r[0] - 1;
    if (c >= CHANNELS_MAX) {
        ros_swi_fail(s, ros_error(ERR_BAD_CHANNEL, "Channel number must be in the range 1-8"));
        return;
    }
    const char *name = s->r[1] >= 0x8000u ? ros_ptr(s->r[1]) : "";
    while (*name == ' ')
        name++;
    uint32_t found = 0;
    for (uint32_t v = 1; v <= VOICES_MAX && !found; v++) {
        uint32_t t = title_of(w, v);
        if (!t)
            continue;
        const char *a = ros_ptr(t), *b = name;
        for (;; a++, b++) {
            char x = (uint8_t)*b <= ' ' ? 0 : *b;
            if (*a != x)
                break;
            if (!x) {
                found = v;
                break;
            }
        }
    }
    if (!found) {
        ros_swi_fail(s, ros_error(ERR_BAD_VOICE, "Sound voice must be in the range 0-32"));
        return;
    }
    uint32_t old;
    s_lock();
    os_error *e = attach(w, c, found, &old);
    s_unlock();
    if (e)
        ros_swi_fail(s, e);
}

void ros_thunk_Sound_ControlPacked(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    /* This is packed as the OSWORD packs it. Byte 0 is the channel and
     * bytes 2-3 are the amplitude with its gate. The pitch and duration
     * follow. */
    ros_sound_note(s->r[0] & 0xFFFF,
                   s->r[0] >> 16,
                   (int32_t)(s->r[1] & 0xFFFF),
                   s->r[1] >> 16);
}

void ros_thunk_Sound_Control(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    ros_sound_note(s->r[0], s->r[1], (int32_t)s->r[2], s->r[3]);
}

/* Read a word of the channel's SCCB, at any byte offset in it. R0 is 0 if
 * the channel or offset is bad. That is not an error, as in Sound1. */
void ros_thunk_Sound_ReadControlBlock(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t c = s->r[0] - 1;
    if (s->r[1] >= 255 || c >= CHANNELS_MAX) {
        s->r[0] = 0;
        return;
    }
    uint32_t v;
    memcpy(&v, (const uint8_t *)&w->ch[c] + s->r[1], 4);
    s->r[2] = v;
}

void ros_thunk_Sound_WriteControlBlock(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t c = s->r[0] - 1;
    if (s->r[1] >= 255 || c >= CHANNELS_MAX) {
        s->r[0] = 0;
        return;
    }
    uint32_t v = s->r[2], old;
    s_lock();
    memcpy(&old, (uint8_t *)&w->ch[c] + s->r[1], 4);
    memcpy((uint8_t *)&w->ch[c] + s->r[1], &v, 4);
    s_unlock();
    s->r[2] = old;
}

/* The master pitch. 0 reads it. Otherwise the low 16 bits of R0 are the new
 * pitch, unless it is tuned automatically, as it is from the start. If R0
 * is "AUTO" then R1 selects what to do. 0 reads the automatic tuning state,
 * 1 turns it off and 2 turns it on. The new state is returned in R0 and the
 * old in R1. */
void ros_thunk_Sound_Tuning(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    uint32_t r0 = s->r[0];
    if (r0 == AUTO) {
        uint32_t want = s->r[1], was = (uint32_t)w->autotune + 1;
        s->r[1] = was;
        if (!want || want == was) {
            s->r[0] = want ? want : was;
            return;
        }
        s_lock();
        w->autotune = want != 1;
        if (w->autotune)
            autotune(w);
        else
            w->per5cs = 5 << 12;        /* a buffer a centisecond, as the old one did */
        s_unlock();
        s->r[0] = want == 1 ? 1 : 2;
        return;
    }
    s->r[0] = w->master_pitch;
    if (r0 && !w->autotune)
        w->master_pitch = r0 & 0xFFFF;
}

/* A pitch's increment, as a 32-bit phase's. This is the 16-bit increment
 * shifted left by 16. */
void ros_thunk_Sound_Pitch(struct ros_cpu *s)
{
    struct ws *w = ws_or_fail(s);
    if (!w)
        return;
    if ((int32_t)s->r[0] < 0x8000) {
        uint32_t r = ror(s->r[0] + w->master_pitch, 12) & ~0x00FF0000u;
        s->r[0] = lsr(pitch_tab[r >> 24], r ^ 0xF);
    }
}

/* ---- the modules ----------------------------------------------------------------- */

/* Sound1's init. It makes the workspace, the tables and the fill. */
static os_error *init(struct ros_module *m, const char *tail)
{
    (void)tail;
    void *block;
    if (xos_module_claim(sizeof(struct ws), &block))
        return ros_error(0x1A6, "Sound system not present");
    struct ws *w = block;
    memset(w, 0, sizeof *w);

    /* Sound0's defaults */
    w->channels = 1;                    /* SCLogChannel. Maestro and BASIC's
                                         * VOICES configure more */
    w->buflen = BUF_DEFAULT;
    w->enabled = 2;                     /* on, as the system starts */
    memset(w->images, 0x80, sizeof w->images);

    /* Sound1's: every channel is flushed and has no voice. Channel 1's
     * voice and the volume come from SoundCMOS (loudness 0-7 becomes
     * &01-&7F). */
    for (int c = 0; c < CHANNELS_MAX; c++)
        w->ch[c].flags = SCCB_FLUSH;
    uint8_t cmos = ros_cmos_read(SOUND_CMOS);
    w->ch[0].voice = (uint8_t)((cmos & 0x0F) + 1);
    ros_audio_mute(!(cmos & 0x80));     /* SetSpeaker: bit 7 on */
    w->max_amp = (uint8_t)((cmos & 0x70) | (cmos & 0x70) >> 3 | 1);
    w->master_pitch = DEF_MASTER_PITCH;
    w->autotune = 1;                    /* SoundDMA tells us its rate */
    autotune(w);
    build_log_table(w);
    build_mulaw();
    strcpy(w->null_voice, "NullVoice");

    sound1_ws = w;
    ros_st32(m->private_word, ros_addr(w));
    ros_audio_add(sound_fill);
    return NULL;
}

static void final_shutdown(struct ros_module *m)
{
    struct ws *w = workspace(m);
    if (!w)
        return;
    ros_audio_remove(sound_fill);
    s_lock();
    sound1_ws = NULL;
    s_unlock();
    ros_st32(m->private_word, 0);
    xos_module_free(w);
}

static os_error *final(struct ros_module *m, int fatal)
{
    (void)fatal;
    final_shutdown(m);
    return NULL;
}

static void service(struct ros_module *m, struct ros_cpu *s)
{
    (void)m;
    (void)s;
}

/* ---- the star commands (from s/Sound1, its Messages and CmdHelp) ------------- */

#define ERR_BAD_PARAM   0x20000         /* BadSoundParameter */
#define ERR_AUTOTUNE    0x2000B         /* AutoTuningUnavailable */

static os_error *xswi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, 10 * sizeof r[0]);
    ros_swi(&c, n | ROS_X_BIT);
    memcpy(r, c.r, 10 * sizeof r[0]);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static os_error *write_s(const char *t)
{
    os_error *e = NULL;
    for (; *t && !e; t++) {
        uint32_t r[10] = { (uint8_t)*t };
        e = xswi(XOS_WriteC, r);
    }
    return e;
}

static os_error *new_line(void)
{
    uint32_t r[10] = { 0 };
    return xswi(XOS_NewLine, r);
}

static os_error *bad_parameter(void)
{
    return ros_error(ERR_BAD_PARAM, "Sound command parameters not recognised");
}

/* Read a number with OS_ReadUnsigned in base 10, with no bad terminators.
 * This returns NULL and the value, or the error. *at moves on past the
 * number. */
static os_error *read_number(uint32_t *at, uint32_t flags, uint32_t limit, uint32_t *value)
{
    uint32_t r[10] = { 10 | flags, *at, limit };
    os_error *e = xswi(XOS_ReadUnsigned, r);
    if (!e)
        *at = r[1], *value = r[2];
    return e;
}

/* *Volume <n>: 1-127 */
static os_error *cmd_volume(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t v;
    if (read_number(&tail, 0x80000000u, 0, &v) || v == 0 || v >= 128)
        return bad_parameter();
    uint32_t r[10] = { v };
    return xswi(XSound_Volume, r);
}

/* *Voices. This lists each installed voice. The channels it is attached to
 * are marked 1-8 in their columns, followed by its number and title. */
static os_error *cmd_voices(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)tail, (void)argc;
    struct ws *w = workspace(&sound_module);
    if (!w)
        return ros_error(0x1A6, "Sound system not present");
    os_error *e = write_s("         Voice      Name");
    if (!e)
        e = new_line();
    for (uint32_t v = 1; v <= VOICES_MAX && !e; v++) {
        if (!w->voice[v].svcb)
            continue;
        char line[96];
        size_t n = 0;
        for (unsigned c = 0; c < CHANNELS_MAX; c++)
            line[n++] = w->ch[c].voice == v ? (char)('1' + c) : ' ';
        n += (size_t)snprintf(line + n, sizeof line - n, "  %s%u   ", v <= 9 ? " " : "", v);
        uint32_t t = title_of(w, v);
        while (t && n < sizeof line - 1 && ros_ld8(t) >= 32)
            line[n++] = (char)ros_ld8(t++);
        line[n] = 0;
        e = write_s(line);
        if (!e)
            e = new_line();
    }
    if (!e)
        e = write_s("^^^^^^^^ Channel allocation map");
    return e ? e : new_line();
}

/* *ChannelVoice <channel> <voice index>|<voice name> */
static os_error *cmd_channelvoice(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t c, v;
    if (read_number(&tail, 0x80000000u, 0, &c))
        return bad_parameter();
    if (c == 0 || c > CHANNELS_MAX)
        return ros_error(ERR_BAD_CHANNEL, "Channel number must be in the range 1-8");
    uint32_t at = tail;
    if (read_number(&at, 0x80000000u, 0, &v)) {
        while (ros_ld8(tail) == ' ')     /* the name, from where OS_ReadUnsigned left it */
            tail++;
        uint32_t r[10] = { c, tail };
        return xswi(XSound_AttachNamedVoice, r);
    }
    uint32_t r[10] = { c, v };
    return xswi(XSound_AttachVoice, r);
}

/* *Sound <chan> <amp> <pitch> <duration>: Sound_Control */
static os_error *cmd_sound(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t c, a, p, d;
    if (read_number(&tail, 0x80000000u, 0, &c))
        return bad_parameter();
    if (c == 0 || c > CHANNELS_MAX)
        return ros_error(ERR_BAD_CHANNEL, "Channel number must be in the range 1-8");
    if (read_number(&tail, 0x80000000u, 0, &a) || read_number(&tail, 0x80000000u, 0, &p) ||
        read_number(&tail, 0x80000000u, 0, &d))
        return bad_parameter();
    uint32_t r[10] = { c, a, p, d };
    return xswi(XSound_Control, r);
}

/* *Tuning [+|-]<n> (0 the default), or *Tuning AUTO ON|OFF */
static os_error *cmd_tuning(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m;
    if (argc == 2) {
        char word[8] = { 0 };
        unsigned n = 0;
        for (; n < 4; n++)
            word[n] = (char)(ros_ld8(tail + n) & ~0x20u);
        if (memcmp(word, "AUTO", 4))
            return bad_parameter();
        uint32_t at = tail + 4;
        while (ros_ld8(at) == ' ')
            at++;
        n = 0;
        while (ros_ld8(at) > ' ' && n < 4)
            word[n++] = (char)(ros_ld8(at++) & ~0x20u);
        word[n] = 0;
        if (ros_ld8(at) > ' ')
            return bad_parameter();
        uint32_t want = !strcmp(word, "ON") ? 2 : !strcmp(word, "OFF") ? 1 : 0;
        if (!want)
            return bad_parameter();
        uint32_t r[10] = { AUTO, want };
        os_error *e = xswi(XSound_Tuning, r);
        if (e)
            return e;
        return r[0] == want ? NULL : ros_error(ERR_AUTOTUNE, "Automatic tuning unavailable");
    }
    uint32_t sign = ros_ld8(tail), v;
    if (sign == '-' || sign == '+')
        tail++;
    if (read_number(&tail, 0x80000000u, 0, &v) || v >> 14)
        return bad_parameter();
    int32_t offset = sign == '-' ? -(int32_t)v : (int32_t)v;
    uint32_t r[10] = { 0 };
    xswi(XSound_Tuning, r);
    r[0] = offset ? r[0] + (uint32_t)offset : DEF_MASTER_PITCH;
    return xswi(XSound_Tuning, r);
}

/* *Configure SoundDefault <0|1> <0-7> <1-16>. This sets the SoundCMOS byte.
 * The speaker is in bit 7, the loudness is in bits 4-6 and channel 1's voice
 * less one is in bits 0-3. */
static os_error *cfg_sounddefault(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    if (tail == 0) {
        os_error *e = write_s("SoundDefault <speaker> <volume> <voice>");
        return e ? e : new_line();
    }
    if (tail == 1) {
        uint32_t b = ros_cmos_read(SOUND_CMOS);
        char line[48];
        snprintf(line, sizeof line, "SoundDefault %u %u %u", b >> 7, b >> 4 & 7, (b & 15) + 1);
        os_error *e = write_s(line);
        return e ? e : new_line();
    }
    uint32_t speaker, loud, voice;
    if (read_number(&tail, 0x80000000u, 1, &speaker) ||
        read_number(&tail, 0xA0000000u, 7, &loud) || read_number(&tail, 0xA0000000u, 16, &voice))
        return ros_configure_error(0);
    if (voice == 0)
        return ros_configure_error(0);
    while (ros_ld8(tail) == ' ')
        tail++;
    if (ros_ld8(tail) > ' ')
        return ros_configure_error(0);
    ros_cmos_write(SOUND_CMOS, (uint8_t)(speaker << 7 | loud << 4 | (voice - 1)));
    return NULL;
}

/* ---- SoundDMA's (Sound0's) commands: from its HelpSrc and Messages ------------ */

#define ERR_BAD_STEREO  0x20002         /* BadSoundStereo, M02 */
#define ERR_BAD_GAIN    0x20009         /* BadSoundGain, M03 */

/* DecodeOnOrOff. "On" gives 2, "Off" (or "Of.") gives 1 and anything else
 * gives 0. */
static int on_or_off(uint32_t at)
{
    while (ros_ld8(at) == ' ')
        at++;
    uint8_t c = ros_ld8(at++);
    if (c != 'O' && c != 'o')
        return 0;
    c = ros_ld8(at++);
    if (c == 'N' || c == 'n')
        return ros_ld8(at) > ' ' ? 0 : 2;
    if (c != 'F' && c != 'f')
        return 0;
    c = ros_ld8(at++);
    if (c != 'F' && c != 'f' && c != '.')
        return 0;
    return ros_ld8(at) > ' ' ? 0 : 1;
}

/* *Audio On|Off: Sound_Enable. There is only one controller, so no name is
 * looked for. */
static os_error *cmd_audio(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    int v = on_or_off(tail);
    if (!v)
        return bad_parameter();
    uint32_t r[10] = { (uint32_t)v };
    return xswi(XSound_Enable, r);
}

/* *Speaker On|Off: Sound_Speaker */
static os_error *cmd_speaker(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    int v = on_or_off(tail);
    if (!v)
        return bad_parameter();
    uint32_t r[10] = { (uint32_t)v };
    return xswi(XSound_Speaker, r);
}

/* *Stereo <chan> <pos>. The channel is 1-8. The position is -127..127 with
 * its sign. */
static os_error *cmd_stereo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t c, p;
    if (read_number(&tail, 0x80000000u, 0, &c) || c - 1 >= CHANNELS_MAX)
        return ros_error(ERR_BAD_CHANNEL, "Channel number must be in the range 1-8");
    while (ros_ld8(tail) == ' ')
        tail++;
    uint8_t sign = ros_ld8(tail);
    if (sign == '-' || sign == '+')
        tail++;
    if (read_number(&tail, 0x80000000u, 0, &p) || p > 127)
        return ros_error(ERR_BAD_STEREO, "Stereo position must be in the range -127 to +127");
    uint32_t r[10] = { c, sign == '-' ? (uint32_t)-(int32_t)p : p };
    return xswi(XSound_Stereo, r);
}

/* *SoundGain <gain>. The gain is 0-7. It is kept for the conversion's next
 * buffer. */
static os_error *cmd_soundgain(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    uint32_t v;
    if (read_number(&tail, 0x80000000u, 0, &v))
        return bad_parameter();
    if (v > 7)
        return ros_error(ERR_BAD_GAIN, "Gain value must be in the range 0-7");
    struct ws *w = workspace(&sound_module);
    if (w)
        w->gain = (uint8_t)v;
    return NULL;
}

static const struct ros_command dma_commands[] = {
    { "Audio", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *Audio On|Off",
      "*Audio controls the sound system.", cmd_audio },
    { "Speaker", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *Speaker On|Off",
      "*Speaker controls the loudspeaker.", cmd_speaker },
    { "Stereo", ROS_CMD_INFO(2, 2, 0, 0),
      "Syntax: *Stereo <chan> <pos> where <chan> is 1-8, <pos> is -127(L) to 127(R) "
      "(0 for centre)",
      "*Stereo sets the stereo position of a sound channel.", cmd_stereo },
    { "SoundGain", ROS_CMD_INFO(1, 1, 0, 0),
      "Syntax: *SoundGain <gain> where <gain> is 0-7 for 0dB (default) to +21dB gain, "
      "in 3dB steps",
      "*SoundGain sets the gain for 8-bit mu-law to 16-bit linear sound conversion.",
      cmd_soundgain },
    { 0 },
};

/* Help and syntax, as Sound1's CmdHelp has them. */
static const struct ros_command commands[] = {
    { "Volume", ROS_CMD_INFO(1, 1, 0, 0), "Syntax: *Volume <n>",
      "*Volume sets the audio channel loudness; range 1-127.", cmd_volume },
    { "Voices", ROS_CMD_INFO(0, 0, 0, 0), "Syntax: *Voices",
      "*Voices lists the installed voices and channel allocation.", cmd_voices },
    { "ChannelVoice", ROS_CMD_INFO(2, 2, 0, 0),
      "Syntax: *ChannelVoice <channel> <voice index>|<voice name>",
      "*ChannelVoice attaches a Voice to a Sound Channel.", cmd_channelvoice },
    { "Sound", ROS_CMD_INFO(4, 4, 0, 0), "Syntax: *Sound <chan> <amp> <pitch> <duration>",
      "*Sound makes a foreground (immediate) sound.", cmd_sound },
    { "Tuning", ROS_CMD_INFO(1, 2, 0, 0), "Syntax: *Tuning <tuning> | <auto on|off>",
      "*Tuning <n> manually alters the relative sound system tuning. The value of <n> is "
      "-&offf to &offf, i.e. -16383 to 16383, where 'o' is octave, 'fff' is fraction of "
      "octave. Automatic tuning must be off for this to have effect. *Tuning 0 resets tuning "
      "to default.*Tuning auto on|off enables or disables automatic tuning. Automatic tuning "
      "is on by default.", cmd_tuning },
    { "SoundDefault", ROS_CMD_INFO(0, 3, 0, ROS_CMD_CONFIGURE),
      "Syntax: *Configure SoundDefault <0|1> <0-7> <1-16> (speaker, volume, voice)",
      "*Configure SoundDefault sets default sound channel one parameters.", cfg_sounddefault },
    { 0 },
};

/* Sound1, the voices' chunk, &40180. */
struct ros_module sound1_module = {
    .title = "Sound1",
    .help = "Sound1\t1.11 (01-Oct-26) ROSGD native",
    .init = init,
    .final = final,
    .service = service,
    .commands = commands,
    .swi_chunk = 0x40180,
    .swi_thunks = ros_swi_thunks_Sound1,
    .swi_names = ros_swi_names_Sound1,
    .swi_prefix = "Sound",
};

__attribute__((constructor)) static void sound1_swi_count(void)
{
    sound1_module.swi_count = ros_swi_count_Sound1;
}

/* SoundDMA, the &40140 SWIs, over the same workspace. */
struct ros_module sound_module = {
    .title = "SoundDMA",
    .help = "SoundDMA\t1.88 (01-Oct-26) ROSGD native",
    .init = NULL,                       /* Sound1's init does the work */
    .final = NULL,
    .service = service,
    .commands = dma_commands,
    .swi_chunk = 0x40140,
    .swi_thunks = ros_swi_thunks_SoundDMA,
    .swi_names = ros_swi_names_SoundDMA,
    .swi_prefix = "Sound",
};

__attribute__((constructor)) static void sound_swi_count(void)
{
    sound_module.swi_count = ros_swi_count_SoundDMA;
}
