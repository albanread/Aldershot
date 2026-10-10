# ZLib

A native version of RISC OS 5's ZLib module (`Programmer/ZLibMod`, 0.05) over zlib 1.3.2 (built by `deps/build-imagelibs.sh`). It provides all 38 SWIs at &53AC0, register for register as ZLibMod's `c/cmodule`. RISC OS 5.30 loads it from `!System.Modules`. The BOX has it in the ROM.

| File | Contents |
| --- | --- |
| `zlibmod.c` | The SWIs, stream records, task association and errors. |
| `gz.c` | `ZLib_GZOpen` to `ZLib_GZEOF`, written again over FileSwitch. |
| `zlibmod.h` | The stream block layout and errors. |

## SWIs

| Offset | SWI |
| --- | --- |
| 0, 1 | `Compress`, `Decompress`. Squash's interface (flags in R0, the 56-byte stream block in R1). zlib format, level 9. |
| 2, 3 | `CRC32`, `Adler32`. |
| 4 | `Version`. Returns "1.3.2" here, and "1.2.13.f" on 5.30. |
| 5 to 7, 34 | `ZCompress`, `ZCompress2`, `ZUncompress`, `ZUncompress2`. |
| 8 to 22, 33, 35 to 37 | The stream calls, `deflateInit_` to `inflateGetDictionary`. R0 is the stream block. |
| 23 to 31 | `GZOpen` to `GZEOF`. gzip files. |
| 32 | `TaskAssociate`. |

Errors start at &81F000 (`ErrorBase_ZLib`), with texts from `Resources:$.Resources.ZLib.Messages`. Other SWIs give &81F004 "Unknown ZLib SWI". Otherwise zlib's return code is in R0.

## Design

- A program's `z_stream` is 56 bytes. The host's is larger, so each stream is a record in the RMA holding the host `z_stream`. The block's `state` word is the record's address. Public words are copied in before each call and back after it.
- zlib's allocator is the module's, using RMA blocks.
- A stream made while a Wimp task is current belongs to that task, unless Compress's bit 4 says not. It is ended at Service_WimpCloseDown.
- A gzip header names RISC OS (13) as the operating system, as RISC OS's zlib does.
- gzip files follow zlib 1.2.13's behaviour over OS_Find, OS_GBPB and OS_Args. An 'R' in the mode stores or restores the load, exec, attributes and length in an `AC` extra field. 'x' is ignored, as on RISC OS.

## Differences from RISC OS 5.30

- A program's own `zalloc` and `zfree` are ignored. The BOX cannot call a program's code from zlib's allocator, and no known caller sets them.
- A bad block, buffer or string gives the abort error &80000002 instead of a fault in `/init`.

## Tests

`boot/selftest_zlib.c` checks names, sizes, checksums, 5.30's compressed bytes, the stream calls, errors and 5.30's gzip file byte for byte. `tests/desktop/compress` (`zlib.bas`, `gz.bas`) runs the same programs on RISC OS 5.30 and the BOX, with the same output.

Source followed: ZLibMod on ROOL's GitLab (BSD), `hdr/ZLib`, OSLib's `Hdr.ZLib`.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
