# ksmbd-tools

The BOX's SMB server (`*Share`) is ksmbd-tools (GNU GPL 2 or later), linked with
GLib (GNU LGPL 2.1 or later) and libnl (GNU LGPL 2.1), each **unchanged**:

* ksmbd-tools **3.5.7**: <https://github.com/cifsd-team/ksmbd-tools/releases/download/3.5.7/ksmbd-tools-3.5.7.tar.gz>
* libnl **3.12.0**: <https://github.com/thom311/libnl/releases/download/libnl3_12_0/libnl-3.12.0.tar.gz>
* GLib **2.88.3**: <https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz>

`deps/build-ksmbd.sh` here is how they are built for the box: static, against
musl, for x86-64 and arm64.
