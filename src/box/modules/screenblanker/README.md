# ScreenBlanker

A native C version of the RISC OS 5 Screen Blanker (`Video/Render/ScrBlank`, 2.34). It blanks the screen after the machine has been idle for the time set, and unblanks it on activity. Blanking goes through PaletteV reason 6, and nothing is drawn.

Activity is mouse movement (polled every 20 cs), a key entering the keyboard buffer, a key going down (except the lid closing, key &210), and, after `*BlankTime W`, a character written.

## Commands and SWIs

`*BlankTime [W|O] [Time]` prints the state with no parameters. `Time` is in seconds, up to 262144. `W` claims WrchV and `O` releases it. Messages come from `Resources:$.Resources.ScrBlanker.Messages`.

`ScreenBlanker_Control` (&43100) takes a reason in R0. The call is defined in `api/defs/screenblanker.toml`.

| R0 | Name | Effect |
| --- | --- | --- |
| 0 | Blank | Cancels flashing and blanks. Service_ScreenBlanking is issued first, and a claimant blanks instead. |
| 1 | Unblank | Cancels flashing, unblanks and restarts the time. |
| 2 | Flash | R1 on cs, R2 off cs, R3 forced cycles, R4 flags. |
| 3 | SetTimeout | R1 centiseconds. |
| 4, 5 | ReadTimeout, ReadTimeout2 | R1 in seconds, or in centiseconds. |
| 8, 9 | StrictBlank, StrictUnblank | Blank into or out of standby without the service call. |
| 10 | ReReadTimeout | Reads the time from CMOS again. |

Any other reason gives error &110 (SWI value out of range). Times are kept in ticks of 20 cs, so 1234 cs reads back as 1220. The blanking time is at least 5 seconds, or 0 for never.

Services issued: Service_ScreenBlanking (&A9), Service_ScreenBlanked (&7A) and Service_ScreenRestored (&7B). The last two are issued from a callback.

CMOS Misc1CMOS (&BC): bits 3 to 5 give the time at start (never, 30 s, 1, 2, 5, 10, 15, 30 minutes). Bit 6 claims WrchV at start.

## Differences from RISC OS 5.30

- Flash in standby does nothing. 5.30 returns through the wrong exit.
- The 20 cs tick is counted on TickerV. The original used OS_CallEvery.
- A callback pending when the module dies is dropped. The original's ran on freed workspace.
- Reasons 6 and 7 (a portable's dimming) are not provided, as in 5.30.

Tests: `tests/desktop/sprutils` (the control, ticker, modules and help probes, compared with 5.30 on the farm) and `boot/selftest_sprutils.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
