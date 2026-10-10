/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* rma.h -- the relocatable module area's heap: what OS_Module 6 and 7
 * claim from and free to.
 *
 * Blocks are arena memory, so compiled and native code share them, and
 * each keeps its size, header word included, in the word before its
 * data, where OS_Heap keeps it and where RISC OS code reads it. The RMA
 * is a RISC OS heap (rma.c).
 */
#ifndef ROSGD_RMA_H
#define ROSGD_RMA_H

#include <stdint.h>

/* A block of at least size bytes, 8-aligned; NULL when the RMA is full. */
void *ros_rma_alloc(uint32_t size);

/* Free a block.  0, or -1 if p is not a block this heap handed out. */
int ros_rma_free(void *p);

/* The RMA heap's descriptor, for OS_Heap and OS_Module (heap.h). */
uint32_t ros_rma_heap(void);

/* The RMA's size, dynamic area 1's: where its heap ends, from its base. */
uint32_t ros_rma_size(void);

/* Grow the RMA, as a claim that found no room does (the kernel's
 * DoRMAHeapOpWithExtension), so that need more bytes fit at its top: 0,
 * or -1 if the free pool or its 256 MB cannot give them. */
int ros_rma_grow(uint32_t need);

/* Bytes in use and free, for *RMA-style reports and tests. */
void ros_rma_stats(uint32_t *used, uint32_t *free_bytes);

#endif
