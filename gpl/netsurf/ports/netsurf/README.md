# NetSurf on ROSGD: half Unix, half Wimp

Design 22, section 11 (RISCOSGrandDesign). NetSurf is one POSIX application,
a 32-bit RISC OS task.
- **Its Unix half:** NetSurf's core and libraries, from `third_party/NetSurf`
  unmodified. It is built for x32 against ROSGD's musl, and its C library's
  file and memory calls go through the Unix bridge.
- **Its Wimp half:** a new native RISC OS front end, ROSGD's own code
  (`frontend/`, TARGET `rosgd`), kept simple. See below.

**No curl, no OpenSSL, no sockets in NetSurf.** Its `http:` and `https:`
go through RISC OS's own URL_Fetcher and AcornHTTP (libcurl on a worker
thread) by ROSGD's fetcher, `frontend/fetch_url.c`. Transfers carry on while
NetSurf is paged out of the Wimp. NetSurf is built with `NETSURF_USE_CURL=NO`,
and the one curl header its `content/fetch.c` includes anyway is a stand-in.

## The front end

`!NetSurf`: `!Run`, the program, `!Sprites`, and `Resources` (Messages,
NetSurf's style sheets and pages, its pointers). `NetSurf [url]` puts an icon
on the icon bar and opens a browser window on the URL, else the home page
(`--homepage_url=`, else the welcome page).
- **The window** is a Wimp window with every control. Its toolbar is a
  window nested along the top of the visible area: Back, Forward, Stop,
  Reload, Home (greyed when they cannot act), a writable URL (Return goes
  there) and the status line. The page begins below the toolbar and scrolls
  under it. The toolbar is not flagged a pane: in ROSGD's Wimp, a writable
  icon in a nested pane is never given the caret by a click.
- **Menus:** Menu over a window gives Back, Forward, Reload, Stop, Home and
  New window (greyed as the toolbar is); the icon bar icon's gives New
  window and Quit, and Select on it opens a window. Adjust keeps a menu
  open.
- **The pointer:** over a page it is followed on null events; NetSurf shows
  a link's address in the status line and chooses the pointer, from
  NetSurf's own pointer sprites.
- **Drawing:** each rectangle the Wimp asks for is drawn by NetSurf into a
  32 bpp sprite the size of the visible area, with output switched to it,
  and the sprite is plotted there. The plotters use RISC OS's own calls:
  ColourTrans, OS_Plot for rectangles, Draw for lines, polygons and paths,
  and Font_Paint (blended) for text. Bitmaps are copied into the sprite's
  pixels.
- **Text:** Homerton, Trinity and Corpus, Medium or Bold, upright or
  slanted, in the UTF8 encoding; measured with Font_ScanString.
- **Input:** Select and Adjust are NetSurf's buttons 1 and 2, but Adjust
  over a link downloads it. Keys go to the page, else on to the Wimp.
  Closing the last window leaves the icon.
- **Downloads** go straight into a directory, with no save box to drag:
  `$NetSurf$Downloads` if it is set, else `Downloads` on the HostFS disc
  (`HostFS:$.Downloads`, made if need be), where the host sees them too.
  The file keeps the name the server or the URL gives it (`-1`, `-2` ...
  added rather than replace one), and the status line says how it went.
  Adjust over a link starts one, and so does anything NetSurf cannot show.
- **The task:** `Wimp_PollIdle` wakes it when NetSurf's scheduler has work
  due. JavaScript is on. NetSurf's own log is discarded unless `-V file`.
- **Not yet:** choices, and drags (text selection).

`main.c` (the task), `window.c` (window, toolbar, menus, events), `plot.c`
(sprite, plotters, fonts, bitmaps), `download.c` (downloads), `fetch_url.c`
(the network),
`rosgd.h`, and NetSurf's build files `Makefile`, `Makefile.defaults`,
`Makefile.tools`.

## The fetcher

`fetch_url_register()` adds it for `http:` and `https:` (`fetcher_add`).
- **The request** is the one NetSurf's curl fetcher makes: GET, or POST
  (url-encoded or multipart); NetSurf's headers and those from its options;
  its cookies and Basic credentials from urldb, read as the fetch starts;
  its User-Agent in R6. AcornHTTP's own cookie jar is left out (R0 bit 30).
- **The response** is AcornHTTP's: the head as HTTP/1.0, then the body.
  Each head line goes to NetSurf as curl's would. Set-Cookie also goes to
  NetSurf's jar. Then NetSurf's rules for 304, 3xx, 401 and only_2xx, and
  the body as data, read with `URL_ReadData` at each of NetSurf's fetcher
  polls (1 MB a fetch a poll at most).
- **Errors** are AcornHTTP's own messages; its two timeouts are NetSurf's
  timeouts.
- An abort from inside a callback only marks the fetch, and the fetcher
  finishes it when the callback returns.

## Build and check

    make posix-libc
    ports/netsurf/build.sh          # deps, libs, monkey, httpd, rosgd: build/ports/netsurf
    ports/netsurf/boxcheck.py       # monkey in the box, one boot
    ports/netsurf/boxcheck.py --net # and real sites: the box gets the network
    ports/netsurf/deskcheck.py      # !NetSurf in the box's desktop, the screen saved
    ports/netsurf/deskcheck.py --net --url https://www.riscosopen.org/
    ports/netsurf/drivecheck.py     # !NetSurf driven with the pointer and keys

`build.sh` builds zlib, libpng, libjpeg-turbo, expat and utf8proc from the
tarballs in `deps-macos-intel/src`, and NetSurf's eleven libraries and
nsgenbind from the clones, all with `posix/cc`. The monkey front end is the
Unix half's test: it has no Wimp. It is linked with the fetcher and
`monkey/harness.c`, which adds `SLEEP ms` (NetSurf's scheduler runs while a
page loads). `httpd` (`monkey/httpd.c`) is a small static Linux server for
the checks.

`boxcheck.py` runs, in one box boot:
- a local page (`file:`) with CSS, a PNG and a script;
- httpd, started by `*RunBox`, and then pages over the box's loopback
  through URL_Fetcher: a page with its stylesheet and image, a redirection,
  a cookie set and sent back, the User-Agent, 3 MB of page, a 404's own
  page, a url-encoded and a multipart form, and a refused connection.

With `--net` the box has QEMU's user networking, as `make boot` gives it
(boxrun's `net=True`; its runs are offline otherwise), and NetSurf loads
example.com over http: and https:, RISC OS Open's site through its 301, and
Wikipedia's RISC OS page, shown as secure.

The checks read what monkey plotted and logged, and what httpd logged.

`deskcheck.py` runs `!NetSurf` as the box's first Wimp task, on a test page
(styled text, a box, an image, a script) or `--url`. When the page has
loaded, the task saves the screen (`-snap`, `*ScreenSave`) and quits
(`-quit`). The checks read its log (`-log`: titles, URLs, loads, the
script's console). The screen is kept as `build/ports/netsurf/deskcheck.png`.

`*WimpSlot` needs only the image, about 4.5 MB; the heap and stack are the
program's dynamic area.

`drivecheck.py` drives `!NetSurf` in a headless box with QEMU's virtio
tablet and keyboard (`tests/lib/deskdrive.py`), as a user would: Adjust on
a link and Select on a zip file (both downloaded into the share's
`Downloads`, compared with the originals), the pointer over a link (its
address in the status line), Select on it, Back and
Forward, the window menu, a URL typed, Home, and the icon bar icon, its New
window and Quit. Each step is checked in the task's log; screenshots are
kept in `build/ports/netsurf/drivecheck/shots`.

## The Apple Silicon box (design 26)

    make ARCH=aarch64 posix-libc
    ARCH=aarch64 ports/netsurf/build.sh deps; ARCH=aarch64 ports/netsurf/build.sh libs
    ARCH=aarch64 ports/netsurf/build.sh rosgd     # build/aarch64/ports/netsurf/!NetSurf
    ROSGD_ARCH=aarch64 ports/netsurf/deskcheck.py # in rosgd-vz

The same sources built A64X32 against musl for A64X32
(`posix/build_musl_a64x32.py`) with the pinned clang, and linked by roscc:
`NetSurf,ff8` is 4.8 MB, every object through roscc's addressing lint.
libpng is built without its NEON code (`arm_neon.h` is not among the box's
headers). Nothing in NetSurf or the front end changed.

Checked on 30 September 2026 under rosgd-vz, VZ's NAT:
- `deskcheck.py` on the local test page: PASS (title, load, Duktape's
  console, the screen).
- Driven through the harness (`deskdrive.py --vz --net`: an Obey file
  double-clicked, then Select on the URL bar, Ctrl-U, a URL typed, Return):
  `http://info.cern.ch/hypertext/WWW/TheProject.html` and
  `https://www.riscosopen.org/` (its 301 to `/content/`, the images, the
  style sheets) fetched through URL_Fetcher and AcornHTTP and drawn.
- **Not on arm64 yet:** monkey. Its harness registers itself in a
  constructor, and roscc refuses `.init_array` in an image.
- `deskcheck.py --url` snaps too early for a page from the network: the
  window's first throbber stop, before the fetch starts, counts as the
  load. That is the front end's, on either box.
