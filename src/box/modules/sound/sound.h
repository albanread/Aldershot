/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* sound.h: the old 8-bit voice system, done natively. It does Sound1's job
 * over the SoundDMA's SWIs. It also declares the interface that the voices
 * in modules/sound/voices.c use. */
#ifndef ROSGD_SOUND_H
#define ROSGD_SOUND_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/module.h"

extern struct ros_module sound_module;      /* SoundDMA: the &40140 faces */
extern struct ros_module sound1_module;     /* Sound1: the voices, &40180 */
extern struct ros_module wavesynth_module;  /* the voices, in the ROM's order */
extern struct ros_module stringlib_module;
extern struct ros_module percussion_module;

/* A note, as the queue's dispatch hands it over. The arguments are the
 * registers of Sound_Control.
 *   channel: 1 to 8, in the low nibble.
 *   amp_gate: the low 16 bits are the amplitude, coded as Sound1 codes it.
 *     &101-&17F is gate on. &181-&1FF is a smooth update. &FFF1-&FFFF are
 *     the BBC's -15 to -1. 0, &100 and &180 are off.
 *   pitch: the octave is in the top bits and 1/4096 of an octave in the low
 *     12 bits. &4000 is middle C. Under 256 it is a BBC note number. &8000
 *     and over is a raw increment.
 *   duration: in 5 cs periods, with &FF meaning for ever. Sound1's
 *     Sound_Control takes it the same way. */
void ros_sound_note(uint32_t channel, uint32_t amp_gate,
                    int32_t pitch, uint32_t duration);

/* The number of notes handed to ros_sound_note so far, and how many of
 * them gated a note on. This is for the test harness (the "sound" command
 * in platform/agent_vsock.c). */
void ros_sound_state(uint32_t *in, uint32_t *on);

/* Mix the sounding channels into `frames` stereo frames at out. The sound
 * is added to what is already there, as each of platform.h's fillers adds
 * (#70). This is the second half of the fill, without its tick of the
 * queue. The self-test uses it. */
void ros_sound_mix(int16_t *out, size_t frames);

/* Take the fill off the audio chain (1) or put it back on (0). The
 * self-test uses this so that what ros_sound_mix makes is the test's
 * alone. */
void ros_sound_hold(int held);

/* ---- the interface the voices use: Hdr:Sound's, as C -----------------------
 *
 * A channel's control block, the SCCB, is 256 bytes, as in Sound1.
 * Sound_ReadControlBlock and Sound_WriteControlBlock reach it by offset.
 * The voice also keeps its state in it between fills. The words from Pitch
 * on belong to the voice, which keeps its registers there, as WaveSynth
 * does. */
struct ros_sccb {
    uint8_t amp;                        /* SoundChannelAmpGateB: the 7-bit log amplitude */
    uint8_t voice;                      /* SoundChannelVoiceIndexB, 0 none */
    uint8_t instance;                   /* SoundChannelInstanceB */
    uint8_t flags;                      /* SoundChannelFlagsB, below */
    uint32_t pitch;                     /* the phase in the top 16 bits, the increment a sample in the low 16 */
    uint32_t timbre;
    uint32_t duration;                  /* fills left (signed) */
    uint32_t user[4];                   /* SoundChannelUserParam1-4 */
    uint8_t extension[256 - 32];
};

#define SCCB_GATE_OFF   0x80            /* the flags, in priority order */
#define SCCB_GATE_ON    0x40
#define SCCB_UPDATE     0x20
#define SCCB_ACTIVE     0x08
#define SCCB_OVERRUN    0x04
#define SCCB_FLUSH2     0x02            /* the flush count: buffers to clear */
#define SCCB_FLUSH1     0x01
#define SCCB_FLUSH      0x03

/* What a voice's entry is given. These are the registers of Sound1's
 * Level1Fill. The voice fills the channel's bytes in the DMA buffer, from p
 * up to end, at every step-th byte. They are 8-bit log samples, with bit 0
 * the sign. The structure also holds the tables that the voices scale by. */
struct ros_voice_fill {
    struct ros_sccb *cb;                /* R9 */
    unsigned channel;                   /* R7: 0-7 */
    uint8_t *p, *end;                   /* R12, R10 */
    unsigned step;                      /* R11: the channels interleaved */
    unsigned period;                    /* R8: the sample period, us */
    uint32_t rate;                      /* Sound_SampleRate's, 1/1024 Hz */
    void *inst;                         /* the channel's instance data */
    const uint8_t *log;                 /* SoundLevel1LogTable: linear >> 19 to log */
    const uint8_t *amp;                 /* SoundLevel1AmpTable: a log byte, scaled by
                                         * the volume */
};

/* A voice generator, as the SVCB's entries give it. Each entry fills the
 * buffer and returns the channel's flags. These are SCCB_ACTIVE to go on,
 * or SCCB_FLUSH2 when done. */
struct ros_voice {
    const char *title;                  /* "WaveSynth-Beep" */
    uint32_t dsize;                     /* its instance data, a channel */
    uint8_t (*fill)(struct ros_voice_fill *f);
    uint8_t (*update)(struct ros_voice_fill *f);
    uint8_t (*gate_on)(struct ros_voice_fill *f);
    uint8_t (*gate_off)(struct ros_voice_fill *f);
};

/* Install a voice in the first free slot, as a voice module's
 * Sound_InstallVoice 1,0 does. This returns the slot, or 0 if none was
 * free. The second function takes a voice out. */
uint32_t ros_sound_voice_install(const struct ros_voice *v);
void ros_sound_voice_remove(uint32_t slot);

/* WaveSynth-Beep's generator (voices.c). A voice installed by
 * Sound_InstallVoice plays this one, because its ARM code is not something
 * the box can call. */
extern const struct ros_voice ros_voice_beep;

#endif
