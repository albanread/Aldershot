# The web browser's programs

The BOX's own programs in the WPE edition (MIT, the BOX's licence):

* `ports/compositor/` -- rosgd-compositor, the BOX's compositor: wlroots'
  reference compositor tinywl (MIT, `LICENSE.wlroots`) with the BOX's changes.
* `ports/browser/` -- rosgd-browser, !Browser's engine: one WPE WebKit view.
* `ports/wpe/` -- `wperun`, which runs a program in the Linux root, and
  `mkwpe.py`, which makes the root from Debian's packages.

They use WPE WebKit (GNU LGPL 2.1 and BSD, among others), wlroots, Wayland,
GLib, Mesa and the rest of the root -- Debian's packages, unchanged, dynamically
linked. Their list, versions and licences are in [../../LICENSES/wpe](../../LICENSES/wpe/).
