# ports/compositor -- ROSGD's compositor (design 29, G5)

A wlroots program in the box's Linux root. It is to blend the RISC OS
desktop, the Wimp's surfaces and Linux programs' Wayland surfaces, with
the Wimp as the window manager (design 29, section 10; X1 decided 6
October 2026: Wayland, through wlroots).

## Building: in the box

It is built in the box itself, with Debian's compiler and the libraries
the box runs. There is no cross-compiler: the development root holds them.

    python3 ports/wpe/mkwpe.py --arch arm64 --out build/wpe-dev --extra \
        cage grim busybox build-essential pkgconf libwlroots-0.20-dev \
        wayland-protocols libxkbcommon-dev libpixman-1-dev libwayland-dev

That is 351 packages and a 1.4 GB image. Then boot a box with
`ROSGD_WPE=build/wpe-dev/wpe-root-arm64.ext4` and a share holding this
directory as `src`, `wperun,e1f` and `page.html` (ports/wpe/wayland-check),
and run `*RunBox HostFS::Host.$.wperun /usr/bin/busybox sh /host/run.sh`
with `test-run.sh` as `run.sh`. That script builds the compositor (make,
gcc), starts it headless (pixman), runs MiniBrowser as a Wayland client,
and captures the result with grim to `/host/comp-shot.png`.

## The test: RISC OS's screen and a Wayland window, composited

`test/`: in a box booted with `rosgd.display=compositor` (and
`ROSGD_WPE=build/wpe-dev/...`), with a share holding this directory as
`src`, `wperun,e1f`, `page.html` and the scripts:

    *RunBox HostFS::Host.$.wperun /usr/bin/busybox sh /host/build.sh
    *RunBox HostFS::Host.$.wperun -d /usr/bin/busybox sh /host/start.sh
    *BASIC -quit HostFS::Host.$.Draw       (D.txt, numbered: circles)
    *RunBox HostFS::Host.$.wperun /usr/bin/busybox sh /host/shot.sh

`start.sh` runs the compositor headless with MiniBrowser on it; `shot.sh`
captures the output with grim.

## Where it is

**Step 0, 6 October 2026.** wlroots 0.20.2's tinywl (MIT, LICENSE.wlroots),
with three changes:
- the screencopy protocol, so that a test can capture what it composites;
- xdg-output, so that grim knows the output's place and size;
- a 1280 x 800 mode for an output that has none of its own (headless).

It was built in the box and composited WPE's MiniBrowser, which grim captured.

**Step 1, 6 October 2026: the RISC OS screen as the bottom layer.**
- **RISC OS's side:** booted with `rosgd.display=compositor`, RISC OS draws
  into shared memory and drives no display. /init publishes the memfd as
  `/dev/rosgd-screen`, a link to `/proc/1/fd/<n>`, anew each mode. The
  pointer goes in the screen block, with its image just before it
  (platform/display_drm.c, include/rosgd/screen.h).
- **The compositor's side:** "the RISC OS screen" section of
  rosgd-compositor.c reads it every 20 ms. It converts the shown bank, at
  any depth, to XRGB8888 in the buffer not on show, and gives the scene
  the rows that changed. The desktop is the bottom node, at the largest
  whole scale that fits and centred; the pointer is the top node.
- **Checked:** in the Apple silicon box under VZ, headless: circles drawn by
  BBC BASIC under MiniBrowser's window, composited and captured. The
  box's self-test, in the usual mode, 1832 ok, none failing.

**Step 2, 6 October 2026: the compositor owns the display.**
`test/start-drm.sh` runs it with wlroots' DRM back end
(`WLR_BACKENDS=drm`, `LIBSEAT_BACKEND=noop`: no seat manager, and no
libinput, so input stays RISC OS's). wperun leaves `/dev/dri` visible
when the box was booted with `rosgd.display=compositor`. Under VZ it took
the virtio-gpu (atomic KMS, the connector's preferred 1280 x 1024) and
showed RISC OS's 1280 x 800 screen centred, with MiniBrowser's window
above; grim captured the output.

**Step 3, 6 October 2026: Wayland toplevels as Wimp windows.** Each Linux
program's window is an ordinary Wimp window, with RISC OS's title bar and
icons. The Wimp moves, stacks, resizes and closes it, and windows and
menus in front of it stay in front.

How it works:
- **The Wimp** (modules/wimp/surface.c) has an *external* surface:
  SurfaceBind (Wimp_Extend &5200) with flags bit 1, R3 the compositor's id.
  The Wimp draws such a window's work area black. At each Wimp_Poll it
  makes a table, and writes it to the screen block (screen.h, struct
  ros_screen_ext, after the palette; a sequence count, odd while it
  writes) only when it has changed. For each external window the table
  gives:
  - where the toplevel's top left goes;
  - the rectangles through which the window is seen.
- **WaylandWindows** (modules/waylandwin) is a native task that the desktop
  starts with rosgd.display=compositor. It connects to the compositor's
  socket, /dev/rosgd-wm, and makes a Wimp window for each toplevel. It
  passes input on:
  - the pointer, from Wimp_GetPointerInfo at null events;
  - the buttons, from a click until they are let go. Select is the left
    button, Menu the right, Adjust the middle;
  - the keys, while the window has its invisible caret. Keys the
    compositor does not translate, F12 and its kin above all, go back to
    the Wimp.

  A new size from the adjust size or toggle size icon is sent as a
  configure. The close icon asks the program to close.
- **The compositor** puts the toplevels in a tree under the desktop. The
  desktop is ARGB, opaque except over each external window's rectangles,
  where it is clear, and each toplevel sits where the table says. The
  output's scale is the largest whole one at which the RISC OS screen
  fits, so a layout pixel is a RISC OS pixel. Its keyboard is its own: an
  xkb keymap (us), with RISC OS key codes turned into evdev keys and
  shift or CTRL.

The socket's lines are listed in rosgd-compositor.c, "the Wimp as window
manager".

**Checked:** in the Apple silicon box under VZ, desktop, DRM back end. The
test drove it with tests/lib/deskdrive.py --vz and captured it with grim,
using `test/Go,feb` (build, then start-all.sh: the compositor and two
MiniBrowsers) and `test/Shot,feb`. Results:
- two Linux windows in Wimp windows, stacked as the Wimp stacks them, with
  a task window in front of both;
- page2.html counted the clicks and took "Hello RISC OS" typed at the
  RISC OS keyboard;
- its pointer position followed the work area;
- dragging the title bar moved the Linux window;
- toggle size configured it to 1278 x 779;
- the close icon closed MiniBrowser, and its Wimp window went.

The box's self-test is 1850/1850. `wperun -d` now forks twice, so that
*RunBox's pty closing cannot hang up the program.

Then, in the same step:
- **The scroll wheel.** The windows ask for extended scroll requests
  (extra flags bit 1). The Wimp's wheel (window.c) sends each one a
  Scroll_Request, and the task sends it on as `axis`. Three notches
  scrolled page2.html down 249 pixels.
- **Popups** go in a tree of their toplevel's, above the desktop. That
  tree follows the toplevel, so a menu is not cut off at its window's
  edge.
- **xdg-decoration:** a program that offers to leave its frame to the
  window manager is told to (server side), and the Wimp's furniture is
  the frame.

MiniBrowser uses neither popups nor decorations. foot, a Wayland terminal
(the development root with `--extra ... foot`, test/start-foot.sh),
asks for server-side decoration and got it: one title bar, the Wimp's.
busybox's shell ran in it, with what was typed at the RISC OS keyboard
(shifted characters included). Popups are still unseen: gtk3-demo, which
has them, dies loading its icons, because glycin's image loaders run under
bubblewrap, which needs user namespaces (design 23's R10, as WebKit's
sandbox).

**Still to do:** a popup reaching past its window gets no input outside
the window, since RISC OS sees another window there.

**The box starts it.** A box booted with `rosgd.display=compositor` starts
the compositor itself. WaylandWindows' module initialisation runs
`/usr/bin/wperun -d /usr/bin/rosgd-compositor -b`. wperun is now in the
ROM, both boxes. `-b` sets the box's settings (DRM, pixman, libseat's noop
session) and logs to /run/rosgd-compositor.log in the root. The console
says `rosgd: compositor: ... started`. `rosgd.compositor=PATH` names
another program in the root, for a test's own build, for example
`/host/src/rosgd-compositor`; `=none` starts none. For the edition the
compositor must be built into the root at /usr/bin; the development root
builds it in the box (`test/build.sh`).

**The edition's root has it.** `sh ports/compositor/build-box.sh` builds the
compositor in a headless box, using the development root, which it makes
if it is missing. It leaves build/wpe/rosgd-compositor-aarch64.
`ARCH=aarch64 sh ports/wpe/build.sh` then puts the compositor in the
edition's root at /usr/bin, with libwlroots-0.20; mkwpe.py's `--add` is
how it gets there. That makes 194 packages, 839 MB.

`*WaylandRun <program> [arguments...]` (WaylandWindows) starts a Linux
program in that root, through wperun -d. Checked in the edition box, with
nothing on the share:
- the compositor started by itself;
- `*WaylandRun /usr/lib/aarch64-linux-gnu/wpe-webkit-2.0/MiniBrowser
  https://example.com` opened a Wimp window titled "Example Domain".

wperun now copies the name servers on every run, not only when it first
mounts the root. The compositor's start, at module initialisation, comes
before the network has them.

## The steps to come (design 29's G5a-G5c)

1. **The PC's cards:** the same on nouveau (the T1000), with the TU117
   firmware in the image.
2. **The Wimp's surfaces as scene buffers.** surface.c's layer list --
   placement, clip rectangles, order -- comes over a socket.
3. **GL ES:** the GLES2 renderer in place of pixman, on our QEMU with
   virglrenderer and ANGLE (the Mac) and on the T1000 (the PC).
