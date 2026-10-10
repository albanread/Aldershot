# Input

Input is the keyboard and pointer driver. It is a native module in C
(`input.c`). On a Pi, keys and the mouse reach RISC OS through the HID half
of the USB driver (`HWSupport/USB/USBDriver`: `usbkboard` and `usbmouse`),
which calls KeyV and PointerV. Input makes the same calls with the same
numbers.

Linux evdev devices are the source: virtio-input under QEMU, a PS/2
keyboard and absolute mouse under VMware, or a PC's USB devices. The
platform code (`platform/input_evdev.c`) opens them, watches `/dev/input`
for devices that come and go, and translates Linux key codes to the RISC OS
low level key numbers. Linux autorepeat is dropped, because RISC OS repeats
keys itself.

## Use

There are no commands and no SWIs. The module makes these calls:

| Call | When |
| --- | --- |
| KeyV 0, KeyboardPresent | once, after every module has started |
| KeyV 2 and 1, KeyDown and KeyUp | each key and mouse button |
| PointerV 3, Report | movement (relative), or a tablet position (absolute, in OS units) |
| PointerV 9, WheelChange | wheel movement |

It answers PointerV 0 (Request), 1 (Identify) and 2 (Selected) as pointer
device type 7, the type that the USB driver has.

`rosgd.inputlog` on the kernel command line prints every call it makes.

## Mac keys

A Mac keyboard has no Break key and a Mac mouse has no middle button, so
Input changes three things.

| You press | RISC OS sees |
| --- | --- |
| Ctrl-Cmd-Delete | Break with Ctrl down, which resets the machine |
| Ctrl-click | Menu |
| Cmd-click | Adjust |

The reset itself is done by the kernel (`runtime/keyboard.c`).

## Differences from RISC OS 5.30

This is new code written from the USB driver's calls. A Pi has no evdev,
and the Mac key changes above are not in RISC OS.

## Tests

- `boot/selftest_input.c`.
- `tests/deskprobe/reset.py` and `tests/wimp/cases/macclicks.bas` for the
  Mac keys.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
