# Sound

The old 8-bit voice system and the queue that plays it, so that desktop music programs such as !Maestro can play. It follows ROOL's `HWSupport/Sound`. The modules are native x32 code. They run on SWIs and from the audio thread, where the fill is also the queue's beat.

| Module | Chunk | Source file | SWIs |
| --- | --- | --- | --- |
| `SoundDMA` | &40140 | `sound/sound.c` | Configure, Enable, Stereo |
| `Sound1` | &40180 | `sound/sound.c` | Volume, voices, Control |
| `SoundScheduler` | &401C0 | `soundsched/soundsched.c` | QTempo, QBeat, QSchedule |
| `WaveSynth`, `StringLib`, `Percussion` | none | `sound/voices.c` | install the voices |

BASIC's `SOUND` with a time parameter compiles to `Sound_QSchedule`. `BEAT`, `BEATS` and `TEMPO` use `Sound_QBeat` and `Sound_QTempo`.

## The queue

`Sound_QSchedule` takes a time in beats, counted from the bar's last 0 (from now if no bar is set). A control of 0 is a sound event, fired as `Sound_Control`. A control of &0F000000 + n fires SWI n with R0 = R2 and R1 = R3. This is how Maestro sends MIDI note bytes. Other controls (a code address) are not used. Tempo is in beats per centisecond in 2^12 fixed point, and &1000 is a beat a centisecond. The queue's clock advances on the audio fill, and due events fire in time and insertion order.

## The voices

Voice slots follow the RISC OS 5 ROM: 1 WaveSynth-Beep, 2 to 5 StringLib (Soft, Pluck, Steel, Hard), 6 to 9 Percussion (Soft, Medium, Snare, Noise). Each channel has a 256-byte control block as in Sound1. Output uses Sound0's defaults (22050 Hz, mu-law to linear), then is oversampled 2x to 44100 Hz.

## Not done

- The MIDI module. Maestro runs without it and uses the voice system.
- Calling the code of a voice installed with `Sound_InstallVoice`. It is listed and attached by name, and plays as WaveSynth-Beep.
- BBC envelopes (`ENVELOPE`). Sound1 did not take them either.
- WaveSynth's wavetable files (Brass and the like).

Tests: `tests/sound/voices.py` plays each voice in the BOX, muted, and cuts the capture (`rosgd.soundcapture`) into one WAV a voice. Issue #62 covers the queue's timing fixes.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
