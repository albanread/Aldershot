# libsmb2

BOX's LanManFS module uses libsmb2 (GNU LGPL 2.1 or later) as its SMB2/3 client. It is unchanged, and it is linked statically into `/init`.

- libsmb2, commit fc710a3ebd58a3c15d0ef24322f748e38a3d3a90: <https://github.com/sahlberg/libsmb2/archive/fc710a3ebd58a3c15d0ef24322f748e38a3d3a90.tar.gz>

`deps/build-libsmb2.sh` builds it against musl, with `deps/libsmb2/config.h` in place of the CMake checks.

## Using your own libsmb2

You may replace libsmb2 with a modified version and relink `/init`. The source of `/init` is in `src/box`. Its Makefile takes the library's place from `SMB2_GUEST` (the box) and `SMB2_HOST` (the hosted test): each is a directory with `include/` and `lib/libsmb2.a`. Build your libsmb2 into such a directory, and build `/init` with the Makefile's variable set to it.

Licence: see `LICENSES/box/libsmb2.txt`.
