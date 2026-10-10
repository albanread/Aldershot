# Squash

A native version of RISC OS 5's Squash (`Programmer/Squash`, 0.31). It provides `Squash_Compress` and `Squash_Decompress`, 12-bit LZW in the format of Unix `compress -b 12`. The Desktop and ResourceFS use it for squashed files.

| File | Contents |
| --- | --- |
| `squash.c` | The module and the two SWIs, register for register as `c/compress`. |
| `fast.c` | The fast algorithms (`s/comp_ass`, `s/zcat_ass`), which are ARM code in RISC OS, written again in C. |
| `build/gen/squash/cssr.c`, `zssr.c` | RISC OS's own restartable algorithms. The Makefile copies them from the ROOL sources and compiles them unchanged. |
| `squash.h`, `sq.h` | The module's interface, and the fast algorithms'. |

## SWIs

The chunk is &42700 (`api/defs/squash.toml`). Both SWIs take R0 flags, R1 a workspace, R2 and R3 the input and its size, and R4 and R5 the output and its size. They return R0 status, and R2 to R5 updated.

| Flag bit in R0 | Meaning |
| --- | --- |
| 0 | Go on with the operation in the workspace. Clear starts afresh. |
| 1 | More input follows. |
| 2 | Decompress only: the output will all fit, so use the fast algorithm. |
| 3 | Return the sizes in R0 and R1. R0 must be exactly 8. |

Status 0 is done, 1 is input used up, 2 is output full. With R0 = 8, Compress returns a workspace size of 31744 and Decompress returns 17408. The workspace is the caller's, in the arena. The module keeps no state of its own.

Compress uses the fast algorithm when bits 0 and 1 are clear and the output is large enough (3 + R3 x 3 / 2 <= R5). Otherwise it uses the restartable one. The two write different bytes for the same input once the table fills, and either decompressor reads either output.

Errors (CompressErrors, &920, texts from `Resources:$.Resources.Squash.Messages`): &921 Bad address, &922 Bad input, &923 Bad workspace, &924 Bad parameters. Other SWIs in the chunk give &1E6.

Native code can call `squash_compress` and `squash_decompress` from `squash.h`. They behave as the SWIs but check no addresses.

## Differences from RISC OS 5.30

RISC OS 5.30 reads or writes without end in some cases, and the BOX does not.
- Fast compress with R3 = 0 writes nothing and returns 0.
- Fast decompress into too small an output stops at the end and returns 2.
- Fast decompress of a code not made yet gives Bad input.
- Bits past the end of the input read as zeros.
- Unreadable workspace or input give Bad address. OS_ValidateAddress refuses anything below &4000.

Kept from RISC OS: a continued decompression takes its buffer size from the last decompression call, so two interleaved decompressions corrupt each other. The fast compressor ORs the old bytes of the first output word into its first codes.

## Tests

`tests/desktop/squash` has five probes compared line by line with RISC OS 5.30 on the farm. They cover the header and Messages, every size, every error, nine inputs compressed and decompressed in every way, and every output alignment. `boot/selftest_squash.c` checks the farm's bytes for 30000 bytes of text and 70000 random bytes.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
