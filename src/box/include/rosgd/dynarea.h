/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* dynarea.h -- the free pool, and what counts against it (runtime/dynarea.c).
 *
 * The free pool, dynamic area 6, is the memory the box really has free:
 * Linux's MemAvailable, no more than the slot pool can still give (arena.c).
 * Slots are lazy (a page is memory once touched), so a slot's size takes
 * nothing from it, and what a program touches does. Hosted, where there is
 * no box to ask, it is the machine's memory (OS_ReadMemMapInfo) less what
 * is in use: the dynamic areas, the pages touched in every application
 * slot (the Wimp's nodes in amb.c and a task's own in task.c), and the RMA
 * as far as it has reached.
 */
#ifndef ROSGD_DYNAREA_H
#define ROSGD_DYNAREA_H

#include <stdint.h>

/* The machine's memory, in bytes, and the part of it in the free pool */
uint64_t ros_mem_total(void);
uint32_t ros_freepool_bytes(void);

/* amb.c: every node's memory in use (pages touched), and whether a node is
 * mapped in (application space is then that node, as on 5.30). Also
 * resizing that one, with the result 0, or non-zero if it could not be
 * had */
uint32_t ros_amb_bytes(void);
int ros_amb_mapped(void);
int ros_amb_resize_mapped(uint32_t bytes);

/* A Wimp node's pages changed by `bytes` (negative: fewer), as every AMB
 * grow or shrink goes through the kernel's PMP_PhysOp: Service_MemoryMoved
 * (R0 the change, R2 -1) and OS_DynamicArea 6's resize bit with handle -1
 * (ChangeDyn PMPMemoryMoved) */
void ros_dynarea_pmp_moved(int32_t bytes);

/* task.c: the tasks' own slots' memory in use (pages touched), and
 * resizing the running task's, which is made if it has none and gone at 0 */
uint32_t ros_task_slot_bytes(void);
int ros_task_resize_own(uint32_t bytes);

/* Address space in the dynamic-area range for a window onto a slot
 * (arena.h, ros_slot_window_map; the Worker module's ShareMemory): bytes
 * rounded up to pages, taken as an area's space is, so areas and windows
 * never meet. The result is 0 if there is none. A window in use counts as
 * memory for ros_dynarea_contains (vdu.h), so for OS_ValidateAddress and
 * for the Worker module's buffers. It is not an area: OS_DynamicArea does
 * not list it. Once released, its space may be given to another window. */
uint32_t ros_dynarea_window_reserve(uint32_t bytes);
void ros_dynarea_window_release(uint32_t base);

#endif
