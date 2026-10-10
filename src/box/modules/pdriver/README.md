# PDriver

PDriver is the RISC OS printer driver interface (PRM 3, from page 3-611,
SWI chunk &80140) as a native C module, with one driver whose output is a
PDF. A job goes to a file, or to a printer on the network over IPP, with no
printer driver and no CUPS.

An application prints as it always has. It opens a file, selects a job on
it, gives the rectangles of its workspace that go on the page with a
transformation and position for each, and plots them when the driver asks.
The file it opened gets a PDF.

## Use

- The SWIs are defined in `api/defs/pdriver.toml`.
- `printer:` is a small filing system (`printerfs.c`). Opening it for
  output starts a job, writing appends, and closing sends the job to its
  destination. `printer:Name` names the job. `*Copy file printer:` works.
- The destination is a file. It is the one `*PrintTo file` named, or a
  dated name such as `Print-2026-10-04-172530` in `Printer$Out`, which is
  `HostFS:$.PrintOut` unless set. A file that begins `%PDF-` is typed
  &ADF, so a double click opens it in !PDF.

| Command | Does |
| --- | --- |
| `*Printers` | lists the places a job can go, the current one starred |
| `*PrintTo [file] [<name>]` | sends jobs there, or back to the dated name |
| `*PrintTo ipp://<host>/ipp/print` | sends jobs to that printer |
| `*PrintInfo` | says where the next job goes, the page size and the last job |

For an IPP printer, `*PrintTo` asks the printer what it is and what it
takes. It sends PDF when the printer accepts it, and PWG Raster otherwise,
at the printer's own resolution and colour. Any driverless printer (IPP
Everywhere, AirPrint) works.

## How a page is made

1. `PDriver_GiveRectangle` records a rectangle, its transformation, its
   position in millipoints and the background colour. A page takes up to
   sixteen.
2. `PDriver_DrawPage` starts the writer and hands back the first band.
3. A band is up to 256 rows of one rectangle, drawn into a 32 bpp sprite
   at 180 dpi, so one pixel is one OS unit. VDU output is switched into
   the sprite with `OS_SpriteOp 60` (`runtime/vdu/sprout.c`).
4. `PDriver_GetRectangle` sends the band to the writer and hands back the
   next.
5. `PDriver_EndJob` reads the PDF back and writes it to the application's
   file handle.

The transformation is applied by the PDF matrix when the band is placed on
the page, so the application plots in its own coordinates. The writer is
`pdfsvc/` at the top of the repository, a 64 bit Linux program in Rust
that runs once for each job (`/usr/bin/pdfsvc` in the initramfs). It is
started with `posix_spawn`. A forked child of the box's many threads did
not reliably reach `execv`.

`PDriver_Info` gives driver type 64, version 1.00, 180 dpi, the printer
name "PDF" and a features word that says arbitrary transformations are
accepted. `PDriver_PageSize` gives A4 with a 10 mm margin until
`PDriver_SetPageSize` changes it.

| File | Contents |
| --- | --- |
| `pdriver.c` | the SWIs, the jobs, the bands, the pipe to the writer |
| `printerfs.c` | `printer:` |
| `dest.c` | destinations and the three commands |
| `ipp.c` | the Internet Printing Protocol |

## Differences from RISC OS 5.30 and what is not done

- The page is pixels at 180 dpi. Text is not text in the PDF, and the files
  are large.
- `image/urf` (AirPrint) is not written. A printer that takes neither PDF
  nor PWG Raster is refused, and the message names what it does take.
- A job is sent with the baton held, so the desktop waits while a page
  goes to the printer. Nothing watches a job after the printer has taken
  it.
- ScreenDump, illustrations and the driver private SWIs refuse.
- A position given to `PDriver_GiveRectangle` is measured from the paper's
  bottom left corner. The PRM does not say whether it is the paper's or the
  printable area's corner. This has not been checked against RISC OS 5.30
  with PDriverPS.

## Tests

`boot/selftest_pdriver.c` runs hosted and in the box. It checks
`PDriver_Info` and `PDriver_PageSize`, a whole job whose PDF is read back
and compared pixel for pixel, `printer:` and `*PrintTo`. Setting
`ROSGD_PDRIVER_KEEP=<file>` keeps the PDF. Setting
`ROSGD_PRINTER=ipp://printer.local:631/ipp/print` makes the test print a
page on that printer. Without it nothing is printed.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
