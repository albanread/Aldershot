/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_sound.c: Sound and SoundScheduler: the voice system's SWIs
 * as Maestro drives them, and the queue BASIC's SOUND statement feeds.
 * The names are checked to be resolvable too, because that is how Maestro
 * finds MIDI_SoundEnable and how SYS "Sound_..." callers find ours. */
#include <stdio.h>
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"
#include "sound.h"
#include "soundsched.h"

#define check ros_check

/* an RMA buffer for the names that go by register */
static char *name_buf(void)
{
    static char *buf;
    if (!buf)
        buf = (char *)ros_rma_alloc(64);
    return buf;
}

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

/* The number a name resolves to, or 0 if it does not resolve. The name goes
 * by register, so it must live in the arena. It must be an RMA buffer and
 * not the test's own memory. */
static uint32_t number_of(const char *name)
{
    char *buf = name_buf();
    if (!buf)
        return 0;
    strcpy(buf, name);
    uint32_t r[8] = { 0, ros_addr(buf), 0, 0, 0, 0, 0, 0 };
    return swi(XOS_SWINumberFromString, r) ? 0 : r[0];
}

void ros_selftest_sound(void)
{
    struct ros_module *sm = &sound_module, *qm = &soundsched_module,
                      *vm = &sound1_module;

    /* ---- both modules are in the chain, with their chunks ------------- */
    {
        int found = 0;
        for (struct ros_module *c = ros_module_first(); c; c = c->next)
            if (c == sm || c == qm || c == vm)
                found++;
        check(found == 3, "SoundDMA, Sound1 and SoundScheduler are in the chain",
              "%d", found);
        check(sm->swi_chunk == 0x40140, "SoundDMA's chunk is &40140", "%X", sm->swi_chunk);
        check(vm->swi_chunk == 0x40180, "Sound1's is &40180", "%X", vm->swi_chunk);
        check(qm->swi_chunk == 0x401C0, "SoundScheduler's is &401C0", "%X", qm->swi_chunk);
        check(ros_ld32(vm->private_word) != 0 && ros_ld32(qm->private_word) != 0,
              "Sound1's and SoundScheduler's workspaces are claimed at init", NULL);
    }

    /* ---- the names resolve, as SYS callers need them to --------------- */
    check(number_of("Sound_ControlPacked") == 0x40186, "Sound_ControlPacked resolves",
          "%X", number_of("Sound_ControlPacked"));
    check(number_of("Sound_AttachNamedVoice") == 0x4018A, "Sound_AttachNamedVoice resolves",
          "%X", number_of("Sound_AttachNamedVoice"));
    check(number_of("Sound_QSchedule") == 0x401C1, "Sound_QSchedule resolves",
          "%X", number_of("Sound_QSchedule"));
    check(number_of("Sound_QBeat") == 0x401C6, "Sound_QBeat resolves",
          "%X", number_of("Sound_QBeat"));
    check(number_of("MIDI_SoundEnable") == 0, "MIDI_SoundEnable does not (no MIDI module),"
          " Maestro's fallback path", "%X", number_of("MIDI_SoundEnable"));

    /* ---- the configuration SWIs, as Maestro's startup calls them ------ */
    uint32_t r[8];
    {
        r[0] = 0; r[1] = 0; r[2] = 0; r[3] = 0; r[4] = 0;
        check(!swi(XSound_Configure, r), "Configure reads", NULL);
        check(r[0] == 1 && r[1] == 224 && r[2] == 45,
              "the default is Sound0's: 1 channel, 224 samples a buffer, 45 us (22050 Hz)",
              "%u, %u, %u", r[0], r[1], r[2]);
        r[0] = 3; r[1] = 0; r[2] = 0; r[3] = 0; r[4] = 0;
        swi(XSound_Configure, r);
        r[0] = 0;
        check(!swi(XSound_Configure, r) && r[0] == 4, "3 channels are 4, a power of two",
              "%u", r[0]);
        r[0] = 8; r[1] = 0; r[2] = 0; r[3] = 0; r[4] = 0;
        check(!swi(XSound_Configure, r) && r[0] == 4, "Configure(8), as Maestro's, returns the old",
              "%u", r[0]);

        r[0] = 0;
        check(!swi(XSound_Enable, r) && r[0] == 2, "Enable reads on (2)", "%u", r[0]);

        r[0] = 3; r[1] = (uint32_t)-100;             /* channel 3, hard left */
        check(!swi(XSound_Stereo, r) && r[1] == 0, "Stereo(3, -100) returns the old, the centre",
              "%d", (int32_t)r[1]);
        r[0] = 3; r[1] = (uint32_t)-128;
        check(!swi(XSound_Stereo, r) && (int32_t)r[1] == -100, "Stereo(3, -128) reads -100",
              "%d", (int32_t)r[1]);
        r[0] = 3; r[1] = 0;
        swi(XSound_Stereo, r);

        r[0] = 0;
        check(!swi(XSound_Volume, r) && r[0] == 127, "Volume reads 127: SoundCMOS's loudness 7",
              "%u", r[0]);
        r[0] = 60;
        check(!swi(XSound_Volume, r) && r[0] == 127, "Volume(60) returns the old", "%u", r[0]);
        r[0] = 0;
        check(!swi(XSound_Volume, r) && r[0] == 60, "and reads 60", "%u", r[0]);
    }

    /* ---- the voices: RISC OS 5's, in its ROM's order -------------------- */
    {
        static const char *const names[] = {
            "WaveSynth-Beep", "StringLib-Soft", "StringLib-Pluck", "StringLib-Steel",
            "StringLib-Hard", "Percussion-Soft", "Percussion-Medium", "Percussion-Snare",
            "Percussion-Noise",
        };
        r[0] = 0; r[1] = 0; r[2] = 0; r[3] = 0;
        check(!swi(XSound_InstallVoice, r), "InstallVoice 0,0 answers", NULL);
        check(r[1] == 10, "its R1 is the first free slot: 9 voices, so 10", "%u", r[1]);

        int named = 0;
        for (uint32_t v = 1; v <= 9; v++) {
            r[0] = 2; r[1] = v; r[2] = 0; r[3] = 0;
            named += !swi(XSound_InstallVoice, r) && r[2] >= 0x8000u && r[3] >= 0x8000u &&
                     !strcmp(ros_ptr(r[2]), names[v - 1]) && !strcmp(ros_ptr(r[3]), names[v - 1]);
        }
        check(named == 9, "slots 1-9 are WaveSynth-Beep, StringLib-Soft to -Hard, Percussion-Soft"
              " to -Noise, their local names the same", "%d of 9", named);
        r[0] = 0; r[1] = 4;
        check(!swi(XSound_InstallVoice, r) && r[0] >= 0x8000u &&
              !strcmp(ros_ptr(r[0]), "StringLib-Steel"), "InstallVoice 0,4 reads StringLib-Steel",
              "%s", r[0] >= 0x8000u ? (char *)ros_ptr(r[0]) : "");
        r[0] = 0; r[1] = 10;
        check(!swi(XSound_InstallVoice, r) && r[0] == 0, "slot 10 is empty", "%X", r[0]);
        r[0] = 0; r[1] = 33;
        check(swi(XSound_InstallVoice, r), "slot 33 is past MaxNVoices: an error", NULL);

        r[0] = 1; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] >> 8 & 0xFF) == 1,
              "channel 1 starts with voice 1 (SoundCMOS's)", "%X", r[2]);
        r[0] = 2; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] >> 8 & 0xFF) == 0,
              "channel 2 with none", "%X", r[2]);

        r[0] = 2; r[1] = 3; r[2] = 0;
        check(!swi(XSound_AttachVoice, r) && r[1] == 0, "AttachVoice(2, 3) returns the old, none",
              "%u", r[1]);
        r[0] = 2; r[1] = 0;
        check(!swi(XSound_AttachVoice, r) && r[1] == 3, "AttachVoice(2, 0) returns the old",
              "%u", r[1]);
        r[0] = 2; r[1] = 33;
        check(swi(XSound_AttachVoice, r), "AttachVoice(2, 33) is an error", NULL);

        char *name = name_buf();
        strcpy(name, "StringLib-Steel");
        r[0] = 2; r[1] = ros_addr(name);
        check(!swi(XSound_AttachNamedVoice, r), "AttachNamedVoice(2, StringLib-Steel)", NULL);
        r[0] = 2; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] >> 8 & 0xFF) == 4,
              "attaches slot 4", "%X", r[2]);
        strcpy(name, "Organ");
        r[0] = 2; r[1] = ros_addr(name);
        check(swi(XSound_AttachNamedVoice, r), "AttachNamedVoice(2, Organ): no such voice", NULL);
    }

    /* ---- a note, directly as Sound_Control takes it: the SCCB ----------- */
    uint32_t c4;
    {
        r[0] = 0x4000;
        check(!swi(XSound_Pitch, r), "Pitch answers", NULL);
        /* middle C, 261.63Hz, one cycle 2^32 of phase at 22050Hz */
        c4 = r[0];
        check(r[0] > 50450000 && r[0] < 51470000, "its step is middle C's at the channels' rate",
              "%u", r[0]);

        r[0] = 1; r[1] = 0x141; r[2] = 0x4000; r[3] = 100;
        check(!swi(XSound_Control, r), "Control: channel 1, amp 65 gated, middle C",
              NULL);
        r[0] = 1; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r), "ReadControlBlock reads the block's word 0",
              NULL);
        check((r[2] & 0xFF) == 65 && (r[2] >> 24 & (SCCB_GATE_ON | SCCB_ACTIVE)),
              "the block has amp 65, gated on (or sounding)", "%X", r[2]);
        r[0] = 1; r[1] = 4;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFFFF) == c4 >> 16,
              "the block has middle C's increment", "%X, not %X", r[2] & 0xFFFF, c4 >> 16);
        r[0] = 1; r[1] = 12;
        check(!swi(XSound_ReadControlBlock, r) && r[2] <= 492 && r[2] > 400,
              "the block has the duration, 100 x 5 cs as 492 buffers (less any filled)",
              "%u", r[2]);
        r[0] = 9; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && r[0] == 0, "channel 9 has no block: R0 0",
              NULL);
    }

    /* ---- Sound1's amplitude coding and BBC pitches (#71) ------------------ */
    {
        uint32_t in0, on0, in1, on1;
        r[0] = 1; r[1] = 0x141; r[2] = 0x4000; r[3] = 100;
        swi(XSound_Control, r);
        ros_sound_state(&in0, &on0);
        r[0] = 1; r[1] = 0x1C1; r[2] = 0x4400; r[3] = 0;
        check(!swi(XSound_Control, r), "Control &1C1: a smooth update", NULL);
        ros_sound_state(&in1, &on1);
        check(in1 == in0 + 1 && on1 == on0, "a smooth update is a note taken, not one gated on",
              "in %u->%u, on %u->%u", in0, in1, on0, on1);
        r[0] = 1; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFF) == 65 &&
              (r[2] >> 24 & (SCCB_UPDATE | SCCB_ACTIVE)),
              "the update keeps amplitude 65, the note sounding", "%X", r[2]);
        r[0] = 0x4400;
        swi(XSound_Pitch, r);
        uint32_t up = r[0];
        r[0] = 1; r[1] = 4;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFFFF) == up >> 16,
              "and its increment &4400's", "%X", r[2]);
        r[0] = 1; r[1] = 0x1C8; r[2] = 0x4400; r[3] = 0;
        swi(XSound_Control, r);
        r[0] = 1; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFF) == 72,
              "an update to amplitude 72 sounds on at 72", "%X", r[2]);

        static const struct { int32_t bbc; uint32_t amp; } bbc[] = {
            { -15, 0x7F }, { -8, 0x63 }, { -1, 0x47 },
        };
        for (unsigned i = 0; i < sizeof bbc / sizeof bbc[0]; i++) {
            r[0] = 1; r[1] = (uint32_t)bbc[i].bbc; r[2] = 53; r[3] = 1;
            swi(XSound_Control, r);
            r[0] = 1; r[1] = 0;
            check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFF) == bbc[i].amp &&
                  (r[2] >> 24 & (SCCB_GATE_ON | SCCB_ACTIVE)),
                  "a BBC amplitude is Sound1's 7-bit one, gated on", "%d: %X, not %X",
                  bbc[i].bbc, r[2], bbc[i].amp);
        }
        r[0] = 1; r[1] = 0; r[2] = 53; r[3] = 1;
        swi(XSound_Control, r);
        r[0] = 1; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && !(r[2] & 0xFF), "BBC amplitude 0 is off",
              "%X", r[2]);
        r[0] = 1; r[1] = 5; r[2] = 53; r[3] = 1;
        ros_sound_state(&in0, &on0);
        swi(XSound_Control, r);
        ros_sound_state(&in1, &on1);
        check(in1 == in0, "a BBC envelope (amplitude 1-&FF) is no note, as Sound1 takes it", NULL);

        /* OS_Word 7, BASIC's SOUND without a time: the kernel's OsWord07,
         * its block Sound_ControlPacked's R0 and R1 */
        uint32_t *blk = (uint32_t *)name_buf();
        blk[0] = 1 | 0x150u << 16;
        blk[1] = 0x4000 | 20u << 16;
        r[0] = 7; r[1] = ros_addr(blk);
        check(!swi(XOS_Word, r), "OS_Word 7 answers", NULL);
        r[0] = 1; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFF) == 0x50,
              "OS_Word 7 sounds channel 1 at amplitude &50", "%X", r[2]);

        uint32_t c;
        r[0] = 0x4000; swi(XSound_Pitch, r); c = r[0];
        r[0] = 1; r[1] = 0x17F; r[2] = 53; r[3] = 1;
        swi(XSound_Control, r);
        r[0] = 1; r[1] = 4;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFFFF) == c >> 16,
              "BBC pitch 53 is middle C", "%X, not %X", r[2] & 0xFFFF, c >> 16);
        r[0] = 1; r[1] = 0x17F; r[2] = 53 + 48; r[3] = 1;
        swi(XSound_Control, r);
        r[0] = 1; r[1] = 4;
        check(!swi(XSound_ReadControlBlock, r) && (r[2] & 0xFFFF) >= (c >> 15) - 2 &&
              (r[2] & 0xFFFF) <= (c >> 15) + 2, "BBC pitch 101 is an octave up (48 to the octave)",
              "%X, not about %X", r[2] & 0xFFFF, c >> 15);
    }

    /* ---- the voices themselves, heard by the test alone ------------------- */
    ros_sound_hold(1);                  /* the fill off the audio chain */
    {
        r[0] = 127;
        swi(XSound_Volume, r);
        r[0] = 1; r[1] = 0; r[2] = 0; r[3] = 0; r[4] = 0;
        swi(XSound_Configure, r);       /* one channel: a note's whole range */

        /* a note at 0, then 0.6 s: 26460 frames */
        static int16_t a[26460][2], b[26460][2];
        static const char *const names[] = {
            "Beep", "StringLib-Soft", "StringLib-Pluck", "StringLib-Steel", "StringLib-Hard",
            "Percussion-Soft", "Percussion-Medium", "Percussion-Snare", "Percussion-Noise",
        };
        int sounded = 0, clean = 0, ended = 0;
        for (uint32_t v = 1; v <= 9; v++) {
            r[0] = 1; r[1] = v;
            swi(XSound_AttachVoice, r);
            memset(a, 0, sizeof a);
            ros_sound_mix(&a[0][0], 4410);          /* flushed */
            r[0] = 1; r[1] = 0x17F; r[2] = 0x4000; r[3] = 8;   /* 0.4 s */
            swi(XSound_Control, r);
            memset(a, 0, sizeof a);
            ros_sound_mix(&a[0][0], 26460);
            int32_t peak = 0, tail = 0;
            for (int i = 0; i < 26460; i++) {
                int32_t x = a[i][0] < 0 ? -a[i][0] : a[i][0];
                if (i < 17640 && x > peak)
                    peak = x;
                if (i >= 22050 && (a[i][0] || a[i][1]))
                    tail++;
            }
            /* one channel, centred: Sound0's 3 parts of 6, x 11/8, of 3952 */
            sounded += peak > 4000;
            clean += peak <= 16302 && a[0][0] == a[0][1];
            ended += tail == 0;
            if (peak <= 4000 || peak > 16302 || tail)
                ros_check(0, "a voice sounds its note", "%s: peak %d, %d frames after its end",
                          names[v - 1], peak, tail);
        }
        check(sounded == 9, "each of the 9 voices sounds a note at amplitude 127", "%d", sounded);
        check(clean == 9, "within one centred channel's range (16302 a side), the sides equal",
              "%d", clean);
        check(ended == 9, "and is over 0.1 s after its 0.4 s (release, or cut)", "%d", ended);

        /* the Beep: an attack, the decay to &70, the slow fall; at middle C */
        r[0] = 1; r[1] = 1;
        swi(XSound_AttachVoice, r);
        ros_sound_mix(&a[0][0], 4410);
        r[0] = 1; r[1] = 0x17F; r[2] = 0x4000; r[3] = 8;
        swi(XSound_Control, r);
        memset(a, 0, sizeof a);
        ros_sound_mix(&a[0][0], 26460);
        int32_t early = 0, top = 0, late = 0, cross = 0;
        for (int i = 0; i < 26460; i++) {
            int32_t x = a[i][0] < 0 ? -a[i][0] : a[i][0];
            if (i < 441)
                early = x > early ? x : early;              /* the first 10 ms */
            else if (i >= 1764 && i < 3528)
                top = x > top ? x : top;                    /* 40-80 ms */
            else if (i >= 13230 && i < 15435)
                late = x > late ? x : late;                 /* 300-350 ms */
            if (i >= 4410 && i < 13230 && a[i - 1][0] < 0 && a[i][0] >= 0)
                cross++;                                    /* 100-300 ms */
        }
        check(early < top / 2 && late < top && late > top / 3,
              "the Beep attacks, then decays and falls slowly (WaveTable0's envelope)",
              "peaks %d, %d, %d", early, top, late);
        check(cross >= 51 && cross <= 54, "at middle C: 261.6 Hz, 52.3 cycles in 0.2 s",
              "%d crossings", cross);

        /* A string's pluck is the same each time. Its noise's seed is the same,
         * and the instance is fresh, so the phase is 0 too. Its first buffer
         * is silent. A drum's pitch is its own, whatever the note's. */
        static const struct { uint32_t voice, pitch[2]; const char *what; } same[] = {
            { 3, { 0x4000, 0x4000 }, "each pluck of a note is the same pluck (StringLib-Pluck)" },
            { 8, { 0x4000, 0x5800 }, "Percussion-Snare sounds the same at any pitch" },
        };
        for (unsigned t = 0; t < 2; t++) {
            int16_t (*m[2])[2] = { a, b };
            int lead[2];
            for (int k = 0; k < 2; k++) {
                r[0] = 1; r[1] = same[t].voice;
                swi(XSound_AttachVoice, r);
                ros_sound_mix(&a[0][0], 4410);
                r[0] = 1; r[1] = 0x17F; r[2] = same[t].pitch[k]; r[3] = 4;
                swi(XSound_Control, r);
                memset(m[k], 0, sizeof a);
                ros_sound_mix(&m[k][0][0], 13230);
                for (lead[k] = 0; lead[k] < 4000 && !m[k][lead[k]][0]; lead[k]++)
                    ;
            }
            if (t == 0)
                check(lead[1] >= 440, "StringLib's first buffer is silent", "%d frames", lead[1]);
            check(lead[0] < 4000 && lead[1] < 4000 &&
                  !memcmp(&a[lead[0]], &b[lead[1]], sizeof a[0] * 6000), same[t].what, NULL);
        }

        /* the volume scales every voice: 127 - 60 is 67 log steps, 25 dB */
        r[0] = 1; r[1] = 1;
        swi(XSound_AttachVoice, r);
        r[0] = 60;
        swi(XSound_Volume, r);
        ros_sound_mix(&a[0][0], 4410);
        r[0] = 1; r[1] = 0x17F; r[2] = 0x4000; r[3] = 8;
        swi(XSound_Control, r);
        memset(a, 0, sizeof a);
        ros_sound_mix(&a[0][0], 4410);
        int32_t soft = 0;
        for (int i = 0; i < 4410; i++)
            soft = a[i][0] > soft ? a[i][0] : soft;
        check(soft > top / 30 && soft < top / 12, "Volume 60 is about 25 dB down",
              "%d against %d", soft, top);
        r[0] = 127;
        swi(XSound_Volume, r);
    }

    /* ---- the mix adds to the other fillers' sound (#70) ------------------ */
    {
        r[0] = 1; r[1] = 0; r[2] = 0x4000; r[3] = 1;
        check(!swi(XSound_Control, r), "Control: amplitude 0 stops channel 1", NULL);
        static int16_t drain[4410][2];
        ros_sound_mix(&drain[0][0], 4410);  /* the note to its end, the buffers flushed */
        int16_t out[4][2];
        for (int i = 0; i < 4; i++)
            out[i][0] = 1000, out[i][1] = -1000;
        ros_sound_mix(&out[0][0], 4);
        int kept = 1;
        for (int i = 0; i < 4; i++)
            kept &= out[i][0] == 1000 && out[i][1] == -1000;
        check(kept, "with no note sounding, the mix leaves SharedSound's samples as they were",
              "%d,%d", out[0][0], out[0][1]);
    }
    ros_sound_hold(0);

    /* ---- SoundLog and LogScale: Sound1's tables, at the volume ----------- */
    {
        r[0] = 3952u << 19;
        check(!swi(XSound_SoundLog, r) && r[0] == 0xFE, "SoundLog: full scale is log &FE",
              "%X", r[0]);
        r[0] = 0u - (3952u << 19);
        check(!swi(XSound_SoundLog, r) && r[0] == 0xFF, "and its negative &FF (bit 0 the sign)",
              "%X", r[0]);
        r[0] = 0xFE;
        check(!swi(XSound_LogScale, r) && r[0] == 0xFE, "LogScale at volume 127 keeps &FE",
              "%X", r[0]);
        r[0] = 60;
        swi(XSound_Volume, r);
        r[0] = 0xFE;
        check(!swi(XSound_LogScale, r) && r[0] == 0xFE - 2 * 67, "at volume 60, 67 steps down",
              "%X", r[0]);
        r[0] = 127;
        swi(XSound_Volume, r);
    }

    /* ---- the queue: what BASIC's SOUND statement feeds ------------------ */
    {
        r[0] = 0;
        check(!swi(XSound_QInit, r) && r[0] == 0, "QInit answers", NULL);

        r[0] = 64;
        check(!swi(XSound_QBeat, r) && r[0] == 0, "QBeat(64) sets the bar", "%u", r[0]);
        r[0] = -1;
        check(!swi(XSound_QBeat, r) && r[0] == 64, "QBeat(-1) reads it", "%u", r[0]);

        r[0] = 256;                       /* a sixteenth of a beat per cs
                                         * (&1000 is one): a beat every 16cs */
        check(!swi(XSound_QTempo, r) && r[0] == 0x1000, "QTempo reads the default",
              "%X", r[0]);

        r[0] = 0;
        check(!swi(XSound_QFree, r) && r[0] == 512, "QFree starts full", "%u", r[0]);

        /* a note at beat 0, as BASIC packs SOUND 1,65|256,&4000,100,time */
        r[0] = 0; r[1] = 0;
        r[2] = 1 | (65u << 16) | (1u << 24);
        r[3] = (100u << 16) | 0x4000;
        check(!swi(XSound_QSchedule, r) && r[0] == 0, "QSchedule takes a note", NULL);
        r[0] = 0;
        check(!swi(XSound_QFree, r) && r[0] == 511, "QFree counts it", "%u", r[0]);

        /* silence the channel first, so firing is what is heard */
        r[0] = 1; r[1] = 0; r[2] = 0; r[3] = 0;
        swi(XSound_Control, r);

        uint32_t base = ros_soundq_beat();
        r[0] = 16;                        /* one whole beat at this tempo */
        check(!swi(XSound_QSDispatch, r), "QSDispatch advances", NULL);
        r[0] = 0;
        check(!swi(XSound_QFree, r) && r[0] == 512, "the note has fired and freed",
              "%u", r[0]);
        r[0] = 1; r[1] = 0;
        check(!swi(XSound_ReadControlBlock, r), "ReadControlBlock after the fire", NULL);
        check((r[2] & 0xFF) == 65 && (r[2] >> 24 & (SCCB_GATE_ON | SCCB_ACTIVE)),
              "the queue's note sounded on channel 1", "%X", r[2]);

        /* the beat counter advanced with the dispatch */
        check(ros_soundq_beat() == (base + 1) % 64, "the beat advanced one",
              "base=%u now=%u", base, ros_soundq_beat());

        /* a negative time syncs with the last scheduled */
        r[0] = (uint32_t)-1; r[1] = 0;
        r[2] = 2 | (80u << 16) | (1u << 24);
        r[3] = (50u << 16) | 0x4000;
        check(!swi(XSound_QSchedule, r) && r[0] == 0, "QSchedule takes a synced note",
              NULL);
        r[0] = 16;                        /* the next beat passes */
        check(!swi(XSound_QSDispatch, r), "QSDispatch advances again", NULL);
        r[0] = 0;
        check(!swi(XSound_QFree, r) && r[0] == 512, "the synced note fired too",
              "%u", r[0]);

        /* RISC OS's own unit (s/Sound2, the PRM): at DefaultTempo, &1000,
         * a centisecond is one beat, a second a hundred (#62: this box once
         * counted 2^19 a beat, 128 times slow, and Maestro played nothing) */
        r[0] = 0x1000;
        swi(XSound_QTempo, r);
        uint32_t b0 = ros_soundq_beat();
        r[0] = 1;
        swi(XSound_QSDispatch, r);
        uint32_t b1 = ros_soundq_beat();
        r[0] = 100;
        swi(XSound_QSDispatch, r);
        uint32_t b2 = ros_soundq_beat();
        check(b1 == (b0 + 1) % 64 && b2 == (b1 + 100) % 64,
              "QTempo &1000 is a beat a centisecond, as RISC OS's: 1cs one beat, 100cs a hundred",
              "b0=%u b1=%u b2=%u", b0, b1, b2);

        /* The beat counter is Sound2's QBeat (s/Sound2, the PRM's
         * Sound_QBeat). It counts only in a bar, from 0, round at the bar's
         * length, and a new length goes on from where it stands. A time is
         * from the bar's last 0, or from now with no bar. (In #62 it was the
         * queue's clock modulo the bar. So Maestro's lead-in bar started at
         * whatever phase the clock had reached, and a time signature's bar
         * jumped the phase, which left a gap in the music.) The tempo is 1,
         * which is a beat in 4096 cs, so that the fills' own ticks, which
         * run as this does, move nothing while it looks. */
        r[0] = 0;
        swi(XSound_QInit, r);
        r[0] = 1;
        swi(XSound_QTempo, r);
#define BEATS(n) (4096u * (n))          /* the centiseconds n beats take at tempo 1 */
        r[0] = BEATS(100);
        swi(XSound_QSDispatch, r);
        check(ros_soundq_beat() == 0, "with no bar the beat counter stays at 0",
              "%u", ros_soundq_beat());
        r[0] = 5; r[1] = 0;              /* a note 5 beats from now */
        r[2] = 1 | (65u << 16) | (1u << 24);
        r[3] = (1u << 16) | 0x4000;
        swi(XSound_QSchedule, r);
        r[0] = BEATS(4);
        swi(XSound_QSDispatch, r);
        uint32_t free4 = (r[0] = 0, swi(XSound_QFree, r), r[0]);
        r[0] = BEATS(1);
        swi(XSound_QSDispatch, r);
        uint32_t free5 = (r[0] = 0, swi(XSound_QFree, r), r[0]);
        check(free4 == 511 && free5 == 512, "with no bar a time is from now: 5 beats on",
              "free after 4: %u, after 5: %u", free4, free5);
        r[0] = 512;
        swi(XSound_QBeat, r);
        check(ros_soundq_beat() == 0, "a bar begins at 0, whatever the queue's clock",
              "%u", ros_soundq_beat());
        r[0] = BEATS(300);
        swi(XSound_QSDispatch, r);
        r[0] = 384;                      /* a new time signature, mid-bar */
        swi(XSound_QBeat, r);
        uint32_t at300 = ros_soundq_beat();
        struct ros_soundq_state q0, q1, q2;
        ros_soundq_state(&q0);
        r[0] = BEATS(83);
        swi(XSound_QSDispatch, r);
        uint32_t at383 = ros_soundq_beat();
        ros_soundq_state(&q1);
        r[0] = BEATS(1);
        swi(XSound_QSDispatch, r);
        uint32_t at384 = ros_soundq_beat();
        ros_soundq_state(&q2);
        check(at300 == 300 && at383 == 383 && at384 == 0,
              "a new bar length goes on from the count, and it comes round at the length",
              "%u %u %u", at300, at383, at384);
        check(q1.bar_events == q0.bar_events && q2.bar_events == q0.bar_events + 1,
              "Event_Sound is raised as the bar comes round, and not before (#72)",
              "%u, %u, %u", q0.bar_events, q1.bar_events, q2.bar_events);
        r[0] = BEATS(10);
        swi(XSound_QSDispatch, r);
        ros_soundq_state(&q0);
        r[0] = 5;                        /* a bar shorter than the count: cut to 0 */
        swi(XSound_QBeat, r);
        r[0] = BEATS(1);
        swi(XSound_QSDispatch, r);
        ros_soundq_state(&q1);
        check(q1.qbeat == 0 && q1.bar_events == q0.bar_events,
              "a bar cut short by a shorter length goes to 0 without the event, as Sound2's",
              "beat %u, events %u -> %u", q1.qbeat, q0.bar_events, q1.bar_events);
        r[0] = 384;                      /* back to 384, from 0 */
        swi(XSound_QBeat, r);
        r[0] = 384; r[1] = 0;            /* the next bar's start: 384 from this 0 */
        r[2] = 1 | (65u << 16) | (1u << 24);
        r[3] = (1u << 16) | 0x4000;
        swi(XSound_QSchedule, r);
        r[0] = BEATS(383);
        swi(XSound_QSDispatch, r);
        free4 = (r[0] = 0, swi(XSound_QFree, r), r[0]);
        r[0] = BEATS(1);
        swi(XSound_QSDispatch, r);
        free5 = (r[0] = 0, swi(XSound_QFree, r), r[0]);
        check(free4 == 511 && free5 == 512 && ros_soundq_beat() == 0,
              "a time is from the bar's 0: the next bar's note fires as the bar comes round",
              "free before: %u, at: %u, beat %u", free4, free5, ros_soundq_beat());
#undef BEATS
        r[0] = 0x1000;
        swi(XSound_QTempo, r);

        /* a full queue reports itself */
        r[0] = 0;
        swi(XSound_QInit, r);            /* empty, so the count is all ours */
        r[0] = 0; r[1] = 0; r[2] = 1; r[3] = 0;
        int queued = 0;
        for (int i = 0; i < 600; i++) {
            r[0] = 100 + i;
            if (!swi(XSound_QSchedule, r) && r[0] == 0)
                queued++;
            else
                break;
        }
        check(queued == 512, "the queue holds 512 and no more", "%d", queued);
        r[0] = 0; r[1] = 0; r[2] = 0; r[3] = 0;
        swi(XSound_QInit, r);            /* tidy: empty again */
    }

    /* ---- the star commands, Sound1's (#89) --------------------------------- */
    {
        char *buf = name_buf();
        strcpy(buf, "ChannelVoice 1 StringLib-Pluck");
        r[0] = (uint32_t)ros_addr(buf);
        int bad = swi(XOS_CLI, r);
        r[0] = 1; r[1] = 0;
        swi(XSound_AttachVoice, r);     /* reads, and leaves it off */
        uint32_t got = r[1];
        r[0] = 1; r[1] = got;
        swi(XSound_AttachVoice, r);     /* back on */
        check(!bad && got == 3, "*ChannelVoice 1 StringLib-Pluck attaches voice 3 by name", "%u", got);
        strcpy(buf, "ChannelVoice 1 1");
        r[0] = (uint32_t)ros_addr(buf);
        swi(XOS_CLI, r);
        r[0] = 1; r[1] = 0;
        swi(XSound_AttachVoice, r);
        got = r[1];
        r[0] = 1; r[1] = got;
        swi(XSound_AttachVoice, r);
        check(got == 1, "*ChannelVoice 1 1: WaveSynth-Beep again, by number", "%u", got);
        r[0] = 0;
        swi(XSound_Volume, r);
        uint32_t was = r[0];
        strcpy(buf, "Volume 90");
        r[0] = (uint32_t)ros_addr(buf);
        bad = swi(XOS_CLI, r);
        r[0] = was;
        swi(XSound_Volume, r);
        check(!bad && r[0] == 90, "*Volume 90 sets Sound_Volume", "%u", r[0]);
        strcpy(buf, "Sound 9 1 1 1");
        r[0] = (uint32_t)ros_addr(buf);
        check(swi(XOS_CLI, r) && ros_ld32(r[0]) == 0x20001,
              "*Sound on channel 9 is Sound1's bad channel error", "&%X", ros_ld32(r[0]));
        strcpy(buf, "Voices");
        r[0] = (uint32_t)ros_addr(buf);
        check(!swi(XOS_CLI, r), "*Voices lists them", NULL);
    }

    /* ---- the bell, and Escape's silence (#88, #90) -------------------------- */
    {
        uint32_t in0, on0, in1, on1;
        ros_sound_state(&in0, &on0);
        r[0] = 7;
        swi(XOS_WriteC, r);
        ros_sound_state(&in1, &on1);
        check(in1 == in0 + 1 && on1 == on0 + 1,
              "VDU 7 sounds the bell: a note gated on (OS_Byte 211-214's, by OS_Word 7)",
              "notes %u -> %u, gated %u -> %u", in0, in1, on0, on1);
        r[0] = 212; r[1] = 0; r[2] = 0xFF;
        check(!swi(XOS_Byte, r) && r[1] == 0x90, "OS_Byte 212: the bell's information, &90 (loud)",
              "%X", r[1]);

        r[0] = 1000; r[1] = 0; r[2] = 1 | (65u << 16) | (1u << 24); r[3] = (1u << 16) | 0x4000;
        swi(XSound_QSchedule, r);
        struct ros_soundq_state q0, q1;
        ros_soundq_state(&q0);
        r[0] = 125;
        swi(XOS_Byte, r);               /* an Escape */
        r[0] = 126;
        swi(XOS_Byte, r);               /* acknowledged, its effects on */
        ros_soundq_state(&q1);
        check(q0.depth == 1 && q1.depth == 0 && r[1] == 0xFF,
              "acknowledging an Escape empties the sound queue (Osbyte7E's Sound_QInit)",
              "depth %u -> %u, R1 %X", q0.depth, q1.depth, r[1]);
    }
}
