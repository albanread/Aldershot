/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* heap.h -- RISC OS heaps, as OS_Heap manages them (runtime/heap.c).
 *
 * The kernel's heap format and algorithms, on any heap in the arena: the
 * RMA, and every heap a program makes in its own memory.  Addresses are
 * arena addresses; each call is an OS_Heap reason, with its errors.
 */
#ifndef ROSGD_HEAP_H
#define ROSGD_HEAP_H

#include <stdint.h>

#include "rosgd/error.h"

os_error *ros_heap_init(uint32_t hpd, uint32_t size);
os_error *ros_heap_describe(uint32_t hpd, uint32_t *largest, uint32_t *total);
os_error *ros_heap_get(uint32_t hpd, uint32_t size, uint32_t *addr);
os_error *ros_heap_free(uint32_t hpd, uint32_t addr);
os_error *ros_heap_extend_block(uint32_t hpd, uint32_t *addr, int32_t by);
os_error *ros_heap_extend_heap(uint32_t hpd, int32_t by, uint32_t *changed);
os_error *ros_heap_block_size(uint32_t hpd, uint32_t addr, uint32_t *size);
os_error *ros_heap_get_aligned(uint32_t hpd, uint32_t size, uint32_t align, uint32_t boundary,
                               uint32_t skew, uint32_t *addr);

/* Whether [start, end) is memory the arena maps read-write: RAM, which a
 * call may write: the heaps, the slot, zero page, and the part of a dynamic
 * area in use. */
int ros_arena_valid(uint32_t start, uint32_t end);

/* Whether [start, end) may be read: that, and the ROM, which is mapped
 * read-only. OS_ValidateAddress counts the ROM, and a module passes
 * strings in its own image (names, messages), so what only reads takes
 * this, and what writes takes ros_arena_valid. */
int ros_arena_readable(uint32_t start, uint32_t end);

#endif
