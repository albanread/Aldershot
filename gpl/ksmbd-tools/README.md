# ksmbd-tools

BOX's SMB server is ksmbd-tools (GNU GPL 2 or later) with GLib (GNU LGPL 2.1 or later) and libnl (GNU LGPL 2.1), all unchanged:

- ksmbd-tools 3.5.7: <https://github.com/cifsd-team/ksmbd-tools/releases/download/3.5.7/ksmbd-tools-3.5.7.tar.gz>
- libnl 3.12.0: <https://github.com/thom311/libnl/releases/download/libnl3_12_0/libnl-3.12.0.tar.gz>
- GLib 2.88.3: <https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz>

`deps/build-ksmbd.sh` builds them statically against musl, for x86-64 and arm64.
