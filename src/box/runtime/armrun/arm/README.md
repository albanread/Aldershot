# ARM modules the container runs emulated

The ARM container runs ARM programs under an emulation engine. These are RISC OS 5.30's own modules, taken from the 5.30 ROM image and run emulated. They are RISC OS Open's, under the Apache License 2.0.

| File | Contents |
|---|---|
| `SharedCLibrary,ffa` | SharedCLibrary 6.23 (15 May 2024), cut from the 5.30 ROM image at offset `&182320` by `tools/rommodule.py ROM SharedCLibrary OUT`. 221,920 bytes, SHA-256 `be89abf4607165997e00ab8240113d8bec0b2bd2c37aee0a5ac995cbecb15d05` |
| `../../../resources/Resources/!System/Modules/Shadows/MimeMap,ffa` | 5.30's MimeMap 0.19, wrapped by `tools/romwrap.py` |
| `../../../resources/Resources/!System/Modules/Shadows/ZLib,ffa` | 5.30's ZLib 0.05, squeezed, as it is on the 5.30 disc |

ARM programs get their C library from 5.30's SharedCLibrary, run under the engine, and not from the native ROM C library. The native library's entries use the native ABI, which an APCS-32 caller cannot reach without a marshalling layer for each entry.

## Loading

The ROM build of SharedCLibrary is linked at `&FC182320`. When it is copied into the RMA, `runtime/armrun/box.c` (`ros_armrun_builtin_clib`) moves every word that points into the module by the distance the module moved (1,336 words). Then its initialisation runs as the kernel runs it, through `ros_call`.

It is the native SharedCLibrary's built-in ARM shadow ([#147](https://github.com/albanread/A7232ToolChain/issues/147)). It is made at the first ARM reference, and `*NModules` shows it as an `A ... SharedCLibrary (shadow)` line. Only ARM callers reach it. Its SWIs (&80680 to &806BF) from ARM code go to its own SWI handler (`runtime/swi.c`). An x32 program's go to the native module. Its privileged operations (`OS_EnterOS`, `MSR CPSR_c`, CPS, `LDM`/`STM ^`, `MOVS pc`) are modelled by the bridge (`bridge.c`).

## Wrapping other ROM modules

A ROM C module is linked at its ROM address and calls SharedCLibrary in the ROM directly. `tools/romwrap.py` lists the words and branches that the loader (`runtime/module.c`) must put right when the module is copied into the RMA: pointers into the module, the words that address SharedCLibrary's entries (aimed at the emulated library), and each B or BL into SharedCLibrary.

    tools/romwrap.py ROM 'resources/Resources/!System/Modules' MimeMap=Shadows/MimeMap

The loader unsqueezes a squeezed module, as 5.30's kernel does (`ros_module_unsqueeze`, [#162](https://github.com/albanread/A7232ToolChain/issues/162)). Once loaded, the MimeMap and ZLib become shadows of the native modules, for the self-test and `*ARMPrefer`.

5.30's DrawFile and Toolbox modules can be run this way too. The box's ROM now has both natively (`ports/drawfile`, `ports/toolbox`), and the native DrawFile was checked against the emulated 5.30 one.

SharedCLibrary keeps its client modules' static data offsets at the 1 MB-aligned base of the SVC stack. The guard page for runaway SVC stack code is therefore the stack's second page (`ROS_SVCSTACK_GUARD_AT` in `include/rosgd/arena.h`).

RISC OS Open's sources and the ROMs built from them are under the Apache License 2.0 (RISC_OSLib, which SharedCLibrary is built from, included). ZLib carries zlib's own licence for the library.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
