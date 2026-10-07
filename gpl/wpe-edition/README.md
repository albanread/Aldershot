# Web browser programs

BOX's own programs (MIT):

- `ports/compositor/`: the compositor, based on wlroots' tinywl (MIT).
- `ports/browser/`: the browser engine, one WPE WebKit view.
- `ports/wpe/`: `wperun`, and `mkwpe.py`, which builds the Linux root.

They use WPE WebKit, wlroots, Wayland, GLib, Mesa and other libraries, as unchanged Debian packages, dynamically linked. Versions and licences are in [LICENSES/wpe](../LICENSES/wpe/).
