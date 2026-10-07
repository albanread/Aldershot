# NetSurf

NetSurf (GNU GPL 2) and its libraries (MIT), built as a RISC OS application. They are unchanged, from [github.com/netsurf-browser](https://github.com/netsurf-browser) at these commits:

netsurf 39da3c3a4, buildsystem 0005ae3, libwapcaplet c7c128d, libparserutils 6b0cbf0, libhubbub 6651b8c, libdom f69781e, libcss 499f1c4, libnsgif 22e99eb, libnsbmp ea063c9, libnsutils 0bd3906, libnspsl 82815c2, libnslog bedff21, libsvgtiny 073283b, nsgenbind 44c6736.

BOX's own code, under the MIT licence:

- `ports/netsurf/`: the front end, build script and tests.
- `posix/`: the C library NetSurf links with (musl 1.2.6 with BOX's additions).

Also built unchanged: zlib, libpng, libjpeg-turbo, expat and utf8proc, at the versions in [LICENSES](../../LICENSES/README.md).
