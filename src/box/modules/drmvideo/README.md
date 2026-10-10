# DRMVideo

ROSGD's GraphicsV driver, a native module in C (`drmvideo.c`). On a Raspberry
Pi, BCMVideo does this job through the GPU mailbox. DRMVideo uses Linux's DRM
through `platform/display_drm.c`. It is new code, written from GraphicsV as
`hdr/GraphicsV` and the PRM define it.

## How the screen reaches the host

Screen memory is a DRM dumb buffer on virtio-gpu, mapped at `&B4000000`, in the
mode's own format (1 to 32 bits per pixel). The last 4 KB of the buffer is the
screen block (`include/rosgd/screen.h`), which records size, pitch, depth, byte
order, pan and palette. The host's Metal window (`qemu/ui/metal-virtio-gpu.c`)
reads the buffer and the block each frame and decodes the pixels on the GPU. So
nothing is flushed or converted in the guest, and the pointer is DRM's cursor
plane.

Other DRM drivers (vmwgfx, simpledrm) get a converted display. A thread
converts screen memory into the display's XRGB8888 buffer 50 times a second,
scales by a whole number and draws the pointer in software.
`rosgd.display=convert` forces this on virtio-gpu. The hosted build has an
in-memory display (`ROSGD_HOSTED_DISPLAY=WxH`, or `none`).

## GraphicsV reasons

DRMVideo is driver 0 with one head. It claims SetMode (2), SetBlank (4),
UpdatePointer (5), SetDMAAddress (6), VetMode (7), DisplayFeatures (8),
FramestoreAddress (9), WritePaletteEntry (10), WritePaletteEntries (11),
ReadPaletteEntry (12), SelectHead (15), StartupMode (16), PixelFormats (17),
ReadInfo (18) and VetMode2 (19). It does not claim Render, IICOp or the overlay
reasons. Without a display it does not claim GraphicsV at all.

Supported modes are 1, 2, 4 and 8 bpp palettised, 16 bpp as 64K RGB565, and
32 bpp in either byte order. The width and height may not exceed the largest
mode the DRM connector lists. ExtraBytes must be a multiple of 4. Mode flags
other than bits 1, 6, 7, 9, 14 and 15 are refused.

## Differences from RISC OS 5.30

- There is no hardware scroll and no VSync interrupt. The kernel's 50 Hz fake
  VSync paces anything that waits.
- Each SetMode makes a new, cleared framestore at `&B4000000`.
- The 32K 16 bpp format, BGR565 and gamma are not decoded by the host. Gamma
  entries are kept so that they read back.
- Teletext mode flags are passed on, with two screens in the framestore for the
  flashing characters.
- There is no `OS_ScreenMode 64` driver registry yet, so DRMVideo assumes it is
  driver 0. There is one head and no EDID over IICOp.

## Tests

`boot/selftest_graphicsv.c`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
