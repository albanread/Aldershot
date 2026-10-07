# SparkFS

SparkFS 1.50+, David Pilling's, open-sourced through RISC OS Open: the CDDL 1.0
for some files and the Apache License 2.0 for others (each repository's
`LICENCE`; SparkZip adds the Info-ZIP licence).

**Upstream, unchanged**, from `https://gitlab.riscosopen.org/RiscOS/Sources/`:

| Repository | Path | Commit |
| --- | --- | --- |
| SparkFS | FileSys/ImageFS/SparkFS/SparkFS | 62d370c |
| SparkLib | Lib/SparkLib | 2d9fff9 |
| SparkFSApp | Apps/SparkFSApp | f49e0ba |
| SparkFSBin | Apps/SparkFSBin | fad5e0d |
| SparkARJ | FileSys/ImageFS/SparkFS/Codecs/SparkARJ | 522fd85 |
| SparkCab | FileSys/ImageFS/SparkFS/Codecs/SparkCab | bd6e60d |
| SparkCPIO | FileSys/ImageFS/SparkFS/Codecs/SparkCPIO | fc731da |
| SparkLzh | FileSys/ImageFS/SparkFS/Codecs/SparkLzh | 81ab3c1 |
| SparkMcStuffit | FileSys/ImageFS/SparkFS/Codecs/SparkMcStuffit | 3343efa |
| SparkPackdDir | FileSys/ImageFS/SparkFS/Codecs/SparkPackdDir | 5847ecd |
| SparkSpark | FileSys/ImageFS/SparkFS/Codecs/SparkSpark | f03d917 |
| SparkTar | FileSys/ImageFS/SparkFS/Codecs/SparkTar | f786a4c |
| SparkZip | FileSys/ImageFS/SparkFS/Codecs/SparkZip | e318dca |
| SparkZoo | FileSys/ImageFS/SparkFS/Codecs/SparkZoo | b37b72a |

**The BOX's changes**, here, in `ports/sparkfs/`:

* `patches/` -- every change to SparkFS's own files, applied at build time.
* the C in place of each component's assembler (`runlink.c`, `sinterface.c`,
  each codec's `*info.c`, `rminfo.c`), and the BOX's headers (`inc/`).
* `build.sh`, `sparkfs.mk` -- how it is built into the BOX's ROM.
