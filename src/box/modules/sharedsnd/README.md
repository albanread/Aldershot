# SharedSound

The RISC OS sound mixer, as a native C module hand-converted from `Sources/Audio/SharedSnd` in the ROOL tree (originally by Expressive Software Projects). Many clients install linear handlers. Each handler fills or mixes 16-bit stereo samples into one shared buffer. The BOX has one driver, the platform's (`platform/audio_alsa.c`), which calls the fill from its own thread.

## SWIs

The chunk is &4B440 (see `api/defs/sharedsound.toml`): ControlWord, Info, InstallHandler, RemoveHandler, HandlerType, HandlerInfo, SampleRate, HandlerVolume, HandlerSampleType, HandlerPause, InstallDriver, RemoveDriver, DriverInfo, DriverVolume and DriverMixer. The command is `*SharedSound`.

The handler table has 10 slots. A handler is identified by its address and parameter. The fill calls each handler in turn with the original's register contract: R0 flags (bit 0 mix, bit 30 use the R7 volume, bit 31 mute), R1 buffer pointer, R2 buffer end, R4 and R5 the source span, R6 the 16.16 step, R7 volume, R9 the fraction accumulator and R12 the installed parameter. The fraction arithmetic is the original's, integer for integer. An empty table gives silence.

Errors are &B0 (initialisation), &B1 (too many handlers) and &B2 (bad handler).

## Differences from RISC OS 5.30

- The 8-bit logarithmic voice system (`s/Log`, Sound_Configure, voices and envelopes) is not done.
- The original's hardware drivers (IOC, podule, PowerWAVE and the DMA probing) are replaced by the platform driver.
- Service_SharedSoundAlive and Service_SharedSoundDying are not broadcast yet.

Tests: `boot/selftest_sharedsnd.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
