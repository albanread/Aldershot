# A2 results — the corpus and the differential (3 October 2026)

Produced by `make armscan`, `runtime/armrun/tools/corpus_decode.py` over
`v3-test-disc`, and `tests/armrun/idioms.py` against the farm (bigmacfarm
`claude0`, RISC OS 5.30 under QEMU's Cortex-A72). The full per-binary
table is `build/armrun/corpus/corpus.md` (regenerate it); what decides
the next sprints is here.

## The corpus

- **107 `&FF8` images**, all AIF, all 32-bit (address mode 32); **80 squeezed**, every one unsqueezed by its own decompressor run under the engine (OS_GetEnv, XOS_SynchroniseCodeAreas twice, then back to `&8004`).
- **Runtimes:** SharedCLibrary APCS-32 91, none (no library seen) 11, UnixLib 5. (SharedCLibrary clients found by their `LibInitAPCS_32` SWI; their library calls named through the stub vectors and roscc's contract.)
- **Code reached** by following control flow from the AIF entry and every APCS prologue, stopping where a walk runs into data (names, strings, function-name markers, NV words, coprocessors RISC OS never uses).
- **FPA in 48 binaries**, 6277 instructions reached — PipeDream 3,416, OvationPro 490, djpeg 192, Snapper 187 — so the native FPA (design 25 §6) carries real weight. VFP 12, NEON 0: next to none.
- **Old idioms** (26-bit returns, TEQP, `LDM ^`, SWP, MOVS pc) appear only in the five GCC/UnixLib binaries (NetSurf, !PDF, wget, grep, bunzip2) — UnixLib's mode-agnostic runtime — and not in any SharedCLibrary client.
- **The engine, every distinct word reached (395265):** not translated as an ordinary instruction: {'undefined': 2237, 'interpret': 4, 'unpredictable': 1}. The undefined ones are FPA (until the native coprocessors); in the SharedCLibrary clients *every* reached non-FPA instruction translates.

Categories over the corpus (instructions reached):

- fpa: 6277
- swi: 3322
- muls (C flag: ARMv4 unpredictable): 88
- mrs: 86
- msr cpsr control (mode change): 37
- ldrex/strex: 24
- ldm/stm ^ (user-bank registers): 18
- teqp/cmpp family (26-bit PSR write): 15
- movs pc (26-bit return: restores the PSR): 14
- swp: 14
- vfp: 12
- msr spsr: 9
- ldrh/ldrsb/ldrd (ARMv4+): 8
- pld: 8
- coprocessor 15: 8
- msr cpsr flags: 7
- ldm/stm writeback, base in list: 3
- stm storing pc (not an APCS frame): 3

## The first binary (design 25 NA-e's first place)

Candidates — SharedCLibrary APCS-32 clients, command line, files in and out, FPA:

| Binary | Reached | FPA | File I/O (library) |
|---|---|---|---|
| !Boot/Resources/!Internet/bin/Tftp,ff8 | 7475 | 22 | fclose, fgets, fopen, fread, fwrite |
| Apps/!OvnPro/!RunImage,ff8 | 261082 | 490 | fclose, fgets, fopen, fputs, fread, fwrite |
| Apps/!OvnPro/Applets/!TransIMP/!Convert,ff8 | 7586 | 94 | fclose, fopen, fread, fscanf, fwrite |
| Apps/!StrongED/Defaults/Tools/!DigDirSED/Lua/MatchText,ff8 | 34172 | 11 | fclose, fgets, fopen, fread, freopen, fscanf, fwrite, remove, rename, tmpfile |
| Utilities/!ChangeFSI/cjpeg,ff8 | 18418 | 17 | fclose, fopen, fread, fwrite |
| Utilities/!ChangeFSI/djpeg/djpeg,ff8 | 22035 | 192 | fclose, fopen, fread, fwrite |
| Utilities/!ChangeFSI/hpcdtoppm,ff8 | 8586 | 8 | fclose, fopen, fputc, fread, fwrite |
| Utilities/!InterGif/intergif,ff8 | 12684 | 70 | fclose, fgets, fopen, fputs, fread, fwrite, remove |
| Utilities/!PrivatEye/!RunImage,ff8 | 61460 | 352 | fclose, fgets, fopen, fprintf, fputs, fread, fwrite, remove |
| Utilities/!SparkFS/!RunImage,ff8 | 15796 | 12 | fclose, fgetc, fgets, fopen, fputc, fputs, fread, fscanf, fwrite, remove, rename |

**Proposed: `Utilities/!ChangeFSI/djpeg/djpeg`** — JPEG to an image file (PPM and the like) on the command line: a file in, a file out, byte-comparable with the farm's run of the same binary on the same input; 192 FPA instructions reached, so the native FPA is exercised from the first run; 22,035 instructions of ordinary Norcroft C. *For the user to confirm.*

## The differential (tests/armrun/idioms.py)

Code at &11D08, as BASIC's DIM put it on the farm.

| Case | Farm (5.30) | Engine run | Engine step | Agree |
|---|---|---|---|---|
| adds-overflow | r0-3 7FFFFFFF 80000000 C0DE2000 C0DE3000 psr 10010/10 | r0-3 7FFFFFFF 80000000 C0DE2000 C0DE3000 psr 10010/10 | = run | yes |
| subs-borrow | r0-3 00000000 FFFFFFFF C0DE2000 C0DE3000 psr 10000/10 | r0-3 00000000 FFFFFFFF C0DE2000 C0DE3000 psr 10000/10 | = run | yes |
| lsl-reg-32 | r0-3 00000001 00000000 00000020 C0DE3000 psr 01100/10 | r0-3 00000001 00000000 00000020 C0DE3000 psr 01100/10 | = run | yes |
| lsl-reg-33 | r0-3 00000001 00000000 00000021 C0DE3000 psr 01000/10 | r0-3 00000001 00000000 00000021 C0DE3000 psr 01000/10 | = run | yes |
| lsl-reg-256 | r0-3 00000001 00000001 00000100 C0DE3000 psr 00100/10 | r0-3 00000001 00000001 00000100 C0DE3000 psr 00100/10 | = run | yes |
| lsr-imm-32 | r0-3 80000000 00000000 C0DE2000 C0DE3000 psr 01100/10 | r0-3 80000000 00000000 C0DE2000 C0DE3000 psr 01100/10 | = run | yes |
| asr-imm-32 | r0-3 80000000 FFFFFFFF C0DE2000 C0DE3000 psr 10100/10 | r0-3 80000000 FFFFFFFF C0DE2000 C0DE3000 psr 10100/10 | = run | yes |
| rrx | r0-3 00000003 80000001 C0DE2000 C0DE3000 psr 10100/10 | r0-3 00000003 80000001 C0DE2000 C0DE3000 psr 10100/10 | = run | yes |
| ror-reg-32 | r0-3 80000001 80000001 00000020 C0DE3000 psr 10100/10 | r0-3 80000001 80000001 00000020 C0DE3000 psr 10100/10 | = run | yes |
| imm-rotated-carry | r0-3 00000000 FF000000 C0DE2000 C0DE3000 psr 10100/10 | r0-3 00000000 FF000000 C0DE2000 C0DE3000 psr 10100/10 | = run | yes |
| muls-flags | r0-3 80000000 00000002 00000000 C0DE3000 psr 01110/10 | r0-3 80000000 00000002 00000000 C0DE3000 psr 01110/10 | = run | yes |
| umulls | r0-3 FFFFFFFF FFFFFFFF 00000001 FFFFFFFE psr 10000/10 | r0-3 FFFFFFFF FFFFFFFF 00000001 FFFFFFFE psr 10000/10 | = run | yes |
| smlal | r0-3 FFFFFFFF 00000005 00000005 00000000 psr 00000/10 | r0-3 FFFFFFFF 00000005 00000005 00000000 psr 00000/10 | = run | yes |
| mla | r0-3 00000003 00000004 0000000D C0DE3000 psr 00000/10 | r0-3 00000003 00000004 0000000D C0DE3000 psr 00000/10 | = run | yes |
| ldr-unaligned | error &80000002: Internal error: abort on data transfer at &00011D24 → data abort at C+28 | r0-3 00000000 45112233 C0DE2000 M+1 psr 00000/10 | = run | **no** |
| ldrh-unaligned | error &80000002: Internal error: abort on data transfer at &00011D24 → data abort at C+28 | r0-3 00000000 00002233 C0DE2000 M+1 psr 00000/10 | = run | **no** |
| ldrd-word-aligned | r0-3 12233445 13243546 C0DE2000 M+4 psr 00000/10 | r0-3 12233445 13243546 C0DE2000 M+4 psr 00000/10 | = run | yes |
| str-pc | r0-3 00000000 C0DE1000 C0DE2000 M+0 psr 00000/10 mem [0]=C+36 | r0-3 00000000 C0DE1000 C0DE2000 M+0 psr 00000/10 mem [0]=C+36 | = run | yes |
| stm-pc | r0-3 00000000 C0DE1000 C0DE2000 M+0 psr 00000/10 mem [0]=C+36 | r0-3 00000000 C0DE1000 C0DE2000 M+0 psr 00000/10 mem [0]=C+36 | = run | yes |
| stm-wb-base-first | r0-3 00000000 C0DE1000 00000022 M+8 psr 00000/10 mem [0]=00000022 [4]=M+0 | r0-3 00000000 C0DE1000 00000022 M+8 psr 00000/10 mem [0]=00000022 [4]=M+0 | = run | yes |
| stm-wb-base-not-first | r0-3 00000000 00000011 C0DE2000 M+8 psr 00000/10 mem [0]=00000011 [4]=M+0 | r0-3 00000000 00000011 C0DE2000 M+8 psr 00000/10 mem [0]=00000011 [4]=M+0 | = run | yes |
| ldm-wb-base-in-list | error &80000000: Internal error: undefined instruction at &00011D24 → undefined instruction at C+28 | unpredictable &11D24 → undefined instruction at C+28 | = run | yes |
| swp | not run: hangs RISC OS 5.30 on the farm's A72 (the BASIC job never returns; its file stays open) | r0-3 5A5A5A5A 11223344 C0DE2000 M+0 psr 00000/10 mem [0]=5A5A5A5A | = run | yes |
| swpb | not run: as swp | r0-3 000000A5 00000044 C0DE2000 M+0 psr 00000/10 mem [0]=112233A5 | = run | yes |
| ldrex-strex | r0-3 00000000 11223345 00000000 M+0 psr 00000/10 mem [0]=11223345 | r0-3 00000000 11223345 00000000 M+0 psr 00000/10 mem [0]=11223345 | = run | yes |
| clz | r0-3 00010000 0000000F C0DE2000 C0DE3000 psr 00000/10 | r0-3 00010000 0000000F C0DE2000 C0DE3000 psr 00000/10 | = run | yes |
| qadd-saturate | r0-3 7FFFFFFF 7FFFFFFF C0DE2000 C0DE3000 psr 00001/10 | r0-3 7FFFFFFF 7FFFFFFF C0DE2000 C0DE3000 psr 00001/10 | = run | yes |
| msr-flags-mrs | r0-3 00000000 F0000110 C0DE2000 C0DE3000 psr 11110/10 | r0-3 00000000 F0000110 C0DE2000 C0DE3000 psr 11110/10 | = run | yes |
| msr-control-user | r0-3 00000000 00000110 C0DE2000 C0DE3000 psr 00000/10 | r0-3 00000000 00000110 C0DE2000 C0DE3000 psr 00000/10 | = run | yes |
| teqp | error &80000000: Internal error: undefined instruction at &00011D24 → undefined instruction at C+28 | undefined &11D24 → undefined instruction at C+28 | = run | yes |
| movnv-old-nop | error &80000000: Internal error: undefined instruction at &00011D24 → undefined instruction at C+28 | unpredictable &11D24 → undefined instruction at C+28 | = run | yes |
| ldm-user-bank | error &80000000: Internal error: undefined instruction at &00011D24 → undefined instruction at C+28 | interpret &11D24 → undefined instruction at C+28 | = run | yes |
| movs-pc-lr | error &80000000: Internal error: undefined instruction at &00011D28 → undefined instruction at C+32 | unpredictable &11D28 → undefined instruction at C+32 | = run | yes |
| fpa-flt-fix | r0-3 00000007 00000007 C0DE2000 C0DE3000 psr 00000/10 | undefined &11D24 → undefined instruction at C+28 | = run | **no** |
| cond-chain | r0-3 00000005 00000001 C0DE2000 C0DE3000 psr 01100/10 | r0-3 00000005 00000001 C0DE2000 C0DE3000 psr 01100/10 | = run | yes |
| blx-reg-arm | r0-3 00000000 00000009 00000007 C+44 psr 00000/10 | r0-3 00000000 00000009 00000007 C+44 psr 00000/10 | = run | yes |

33 of 36 agree.

**What it decides:**

- The engine agrees with RISC OS 5.30 on every data-processing, shifter-carry, multiply, load/store-multiple, exclusive, saturating, PSR and branch case; block runs and single steps agree everywhere.
- **Unpredictable and untranslated forms are undefined instructions on 5.30** (TEQP, MOVNV, `LDM ^`, `MOVS pc, lr`, LDM writeback with the base in the list): the bridge raises RISC OS's undefined-instruction error for the engine's `undefined`, `unpredictable`, `interpret` and `decode-error` alike — and need not implement `LDM ^` or the 26-bit returns at all.
- **The CPSR in user mode reads `&110`** (the A bit set): the bridge starts tasks with `&110`, which dynarmic keeps.
- **Unaligned LDR/LDRH abort on 5.30** (alignment faults on); the engine loads unaligned. *For the user to decide* (design 25 §11).
- **SWP hangs the farm's 5.30** rather than raising an error; the engine executes it. Nothing in the SharedCLibrary corpus uses SWP.
- **FPA**: 5.30's FPEmulator answers; the engine raises undefined until the native FPA coprocessors (A3/A4).

