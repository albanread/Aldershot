# NetSurf

!NetSurf, the web browser on every BOX's disc: NetSurf (GNU GPL 2) and its
libraries (MIT), built as a RISC OS application for the BOX.

**Upstream, unchanged**, from [github.com/netsurf-browser](https://github.com/netsurf-browser),
at these commits (each library's `git archive` of that commit is what is built):

| Repository | Commit |
| --- | --- |
| netsurf | 39da3c3a4 |
| buildsystem | 0005ae3 |
| libwapcaplet | c7c128d |
| libparserutils | 6b0cbf0 |
| libhubbub | 6651b8c |
| libdom | f69781e |
| libcss | 499f1c4 |
| libnsgif | 22e99eb |
| libnsbmp | ea063c9 |
| libnsutils | 0bd3906 |
| libnspsl | 82815c2 |
| libnslog | bedff21 |
| libsvgtiny | 073283b |
| nsgenbind | 44c6736 |

**The BOX's own code**, here:

* `ports/netsurf/frontend/` -- the RISC OS front end (`TARGET=rosgd`): the
  windows, plotting, the fetcher over RISC OS's URL_Fetcher, downloads.
* `ports/netsurf/build.sh` -- how the libraries and NetSurf are built, and the
  application made.
* `ports/netsurf/monkey/`, the `*check.py` files -- the test harness and checks.
* `posix/` -- the POSIX layer NetSurf is linked against: the BOX's additions to
  musl 1.2.6 (MIT) for its x32 and A64X32 ABIs, and its RISC OS calls.

The image libraries it uses (zlib, libpng, libjpeg-turbo, expat, utf8proc) are
their upstream releases, unchanged, at the versions in
[../../LICENSES](../../LICENSES/README.md).
