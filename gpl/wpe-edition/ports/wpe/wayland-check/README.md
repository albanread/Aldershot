# ports/wpe/wayland-check -- WPE over Wayland, composited by wlroots, in the box

The first proof of RISCOSGrandDesign design 29's X1: Linux programs'
surfaces through Wayland, composited by a wlroots program. No GPU and no
display: wlroots' headless back end and its CPU renderer (pixman).

- `wl.sh` (busybox sh, run in the WPE root by wperun) starts cage, a small
  wlroots kiosk compositor, headless, with WPE's MiniBrowser as its one
  Wayland program showing `page.html`; after 12 s grim captures the
  composited output over Wayland, to `/host/wayland-shot.png`.
- `page.html`: a local page, so no network is needed (CSS gradients,
  rounded corners, a line of JavaScript).

To run it in the Apple silicon box:
1. Build a root with the three extras:
   `python3 ports/wpe/mkwpe.py --arch arm64 --out build/wpe-wl --extra cage grim busybox`
   (17 packages, about 5 MB, more than the plain root).
2. Make a share holding `wperun,e1f` (build/wpe/wperun-aarch64), `page.html`
   and `wl.sh`.
3. Boot a box with `ROSGD_WPE=build/wpe-wl/wpe-root-arm64.ext4`, and run
   `*RunBox HostFS::Host.$.wperun /usr/bin/busybox sh /host/wl.sh`.

6 October 2026, under VZ: the page was drawn by WPE as a Wayland client,
composited by wlroots 0.20 (cage 0.3.1) and captured by grim, all inside
the box. cage reports no Xwayland, which is not wanted, and no cursor
theme. cage and grim are stand-ins for the test: G5's compositor is
ROSGD's own wlroots program, with the Wimp as its window manager.
