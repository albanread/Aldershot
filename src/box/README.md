# BOX: the source of /init

This is the source of `/init`, the program at the centre of BOX. It holds the RISC OS runtime, the platform layer, the boot code and its self-tests, and about 66 modules written for BOX.

## What is here

| Directory | Contents |
| --- | --- |
| `runtime/` | The RISC OS runtime: SWIs, the VDU, memory, modules, tasks, ARM emulation. |
| `platform/` | The layer between the runtime and the host. |
| `boot/` | The start-up code and the self-tests. |
| `include/` | The headers they share. |
| `modules/` | The modules: file systems, networking, graphics, fonts, sound, BASIC and others. |
| `api/` | The SWI definitions that the build turns into headers. |
| `tools/` | The generators and the publication tools. |
| `docs/publish/` | Where each file came from, and under what licence. |

## What is not here

- The original assembler of the modules that BOX builds from it. It is in RISC OS Open's repository, and ROSASM (`src/ROSASM`) translates it.
- The libraries `/init` links: OpenSSL, curl, zlib, libpng, libjpeg-turbo, dynarmic, musl and libsmb2. Their sources are theirs. `gpl/libsmb2` has the source and build script for libsmb2.
- Compiled RISC OS modules, the disc, the applications and the `hal/` kernel.

This directory is a copy. It is made by `tools/publish/export-box.sh` from the project's working tree, and is not edited by hand.

## Licences

Each source file names its licence in its header. Code written for BOX is MIT. Code that follows RISC OS Open's source carries RISC OS Open's licence, and the copyright of that source. `docs/publish/` records the origin of each file.

`/init` links libsmb2 (GNU LGPL 2.1 or later) statically. `gpl/libsmb2/README.md` says how to replace it and relink.

The BOX project asserts no ownership of translated code.
