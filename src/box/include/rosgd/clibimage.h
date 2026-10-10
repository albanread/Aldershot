/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* clibimage.h -- the ROM C library's image, as `roscc link --clib-image`
 * writes it (roscc/src/clib.rs), for the SharedCLibrary module that maps
 * it and the self-test that reads it.
 *
 * It is an ELF32 x86-64 file with OS/ABI 255 and type ET_EXEC, with no
 * entry point. It has one PT_LOAD from file offset 0 at its link address
 * (&72200000, the C ROM: ROS_CROM_BASE), read-only and executable, because
 * the library has no writable data. It also has a PT_NOTE, the "ROSGD" note
 * of version 1 with flags ROS_CLIB_NOTE_FLAG, which an application loader
 * refuses. After the note, 4-aligned, comes the .rosclib header:
 *
 *   +0   "ROSCLIB\0"
 *   +8   format (ROS_CLIB_FORMAT)
 *   +12  the contract's major version (roscc/tools/clibspec/clib-contract.json)
 *   +16  the link address;  +20  the image's size (the PT_LOAD's)
 *   +24  a client's static block: its size, TP's offset in it, its alignment
 *   +36  the number of chunks, then Lib$$Init, RISC OS's (s/initmodule's
 *        LI_ records): {id, entries start, entries end, data start, data end}
 *        a chunk. The entries are one word an entry, its address, in the
 *        contract's order (the stub's entries, then the library-only ones).
 *        The data is the chunk's static template, or 0, 0 if it has none
 *
 * A client's block is its chunks' regions in order, each a copy of the
 * chunk's template, then the thread pointer (TP, the %gs base). TP+0 is the
 * self word (its own address: local-exec code reads %gs:0 for a static's
 * address), TP+4 is the stack limit, and TP+8 and TP+12 are reserved. The
 * library reaches every static at a negative offset from TP. The exported
 * ones (__errno, __iob, __ctype ...) are at the contract's offsets, where
 * the client's stubs define them. clib-rosgd.json, beside the image, says
 * the same, with each entry's and static's name. */
#ifndef ROSGD_CLIBIMAGE_H
#define ROSGD_CLIBIMAGE_H

#include <stdint.h>

#define ROS_CLIB_NOTE_FLAG 4u               /* the ROSGD note's flags: a C library image */
#define ROS_CLIB_MAGIC     "ROSCLIB"        /* and its NUL: 8 bytes */
#define ROS_CLIB_FORMAT    1u

struct ros_clib_header {
    char magic[8];
    uint32_t format, major;
    uint32_t base, size;
    uint32_t block_size, tp_offset, block_align;
    uint32_t chunks;
    /* then `chunks` of: */
};

struct ros_clib_chunk {                     /* Lib$$Init's record */
    uint32_t id;
    uint32_t entries, entries_end;          /* a word an entry */
    uint32_t data, data_end;                /* the template; 0, 0: none */
};

#endif
