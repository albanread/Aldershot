/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* rom.h -- the ROM: compiled modules' areas, where rosasm laid them out.
 *
 * Each image is a module's area as rosasm assembled it, relocated to its
 * base by tools/romimage.py, beside the compiled C for its code. The bytes
 * are still needed. Compiled code reads its tables, strings and error
 * blocks from them at the addresses the source gave them (ADR yields ROM
 * addresses), and RISC OS tests for "in ROM" by address.
 */
#ifndef ROSGD_ROM_H
#define ROSGD_ROM_H

#include <stdint.h>

#include "rosgd/error.h"

struct ros_module;

struct ros_rom_image {
    const char *name;
    const uint8_t *bytes;
    const uint32_t *base, *size;
    void (*register_code)(void);    /* its dispatcher entries */
    int code_only;                  /* no module header: code to call, not start */
    int x32;                        /* an x32 C module (an oslcr client): copied
                                       to the RMA and its code range registered
                                       as it starts, because its statics sit
                                       beside its code and ROM is read-only */
    int shadow;                     /* ROS_SHADOW_* (module.h): 0 never shadowed */
};

/* A native module in the ROM, and its shadow policy (the same column) */
struct ros_rom_native {
    struct ros_module *module;
    int shadow;
};

/* The ROM's contents, in the order the modules start: the native modules
 * first, which are written in C, like the Buffer Manager, then the
 * compiled images. */
extern const struct ros_rom_native ros_native_modules[];

/* The kernel's own module, whose table holds its *commands: first in the
 * chain, as the kernel keeps it (runtime/oscli.c). */
extern struct ros_module ros_utility_module;
extern const unsigned ros_native_module_count;
/* The native Window Manager (modules/wimp), which starts in the compiled
 * one's place unless the box is booted with rosgd.wimp=translated
 * (ROSGD_WIMP=translated hosted). The compiled FilterManager is then left
 * out, because the native Wimp answers its SWIs itself. */
extern struct ros_module *const ros_native_wimp;
extern struct ros_module *const ros_native_filtermgr;
extern const struct ros_rom_image ros_rom_images[];
extern const unsigned ros_rom_image_count;

/* An x32 (A64X32 in the Apple Silicon box) C module's image, as roscc
 * link --module writes it, an ELF32. It is copied into the RMA, its BSS
 * cleared, its RELATIVE fix-ups applied and its code range registered. The
 * results are the module header's address, the RMA block that holds it (to
 * free, with ros_capp_code_remove(base), when the module goes) and its
 * size in memory. name is for errors. */
os_error *ros_module_image_load(const uint8_t *file, uint32_t size, const char *name,
                                uint32_t *base_out, void **block_out, uint32_t *size_out);

/* Keep a placed x32 module's image as it is now, before it has run, to put
 * back for a fresh start (struct ros_module's pristine). */
void ros_module_keep_pristine(struct ros_module *m, uint32_t size);

/* Place every image, make the ROM read-only, register the compiled code,
 * and start each image's module from its header.  A module that fails to
 * start is reported and left out, as RISC OS leaves out a ROM module. */
os_error *ros_rom_init(void);

#endif
