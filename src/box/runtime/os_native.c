/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* os_native.c: the kernel SWIs the runtime implements natively.
 *
 * Each is the x form of a typed call from api/defs/os.toml: it returns an
 * error or NULL, and never sees a register.  The generated thunks
 * (build/gen/api_gen.c) unpack registers for compiled callers.
 *
 * The VDU stream: each character goes through WrchV, as the kernel sends it.
 * A claimant such as a spool, a redirection or a task window therefore sees
 * it. The default owner is the console, until the VDU drivers are compiled
 * from the kernel's source.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/streams.h"
#include "rosgd/ticker.h"
#include "rosgd/vector.h"

#define WRCHV 0x03u
#define RDCHV 0x04u

os_error *xos_write_c(uint8_t ch)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ch;
    if (ros_vector_call(WRCHV, &s))
        return s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
    return ros_wrch_default(ch);
}

os_error *xos_write0(const char *s, uint8_t **end)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        os_error *e = xos_write_c((uint8_t)s[i]);
        if (e)
            return e;
    }
    if (end)
        *end = (uint8_t *)s + n + 1;       /* past the terminator */
    return NULL;
}

void ros_writes(struct ros_cpu *s, uint32_t addr)
{
    for (uint32_t p = addr; ros_ld8(p); p++) {
        os_error *e = xos_write_c(ros_ld8(p));
        if (e)
            ros_raise(e);
    }
    s->v = 0;
}

os_error *xos_new_line(void)
{
    os_error *e = xos_write_c(10);         /* VDU 10, 13, as OS_NewLine */
    return e ? e : xos_write_c(13);
}

/* Reads through RdchV, as the kernel does. Its default owner reads the
 * redirection and *Exec files before the keyboard (streams.c). */
os_error *xos_read_c(uint8_t *ch, int *escape)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    if (ros_vector_call(RDCHV, &s)) {
        *ch = (uint8_t)s.r[0];
        *escape = (int)s.c;
        return s.v ? (os_error *)ros_ptr(s.r[0]) : NULL;
    }
    return ros_rdch_default(ch, escape);
}

/* The Escape condition: bit 6 of the kernel's ESC_Status (OS_Byte 124-126) */
os_error *xos_read_escape_state(int *escape)
{
    *escape = (ros_ld8(ROS_ZEROPAGE + 0x104) & 0x40) != 0;
    return NULL;
}

os_error *xos_generate_error(const os_error *error)
{
    return (os_error *)error;
}

/* Interrupts are background work here: IntOff defers it, IntOn lets it
 * run again (background.h). Enabling interrupts on RISC OS takes any that
 * are pending at once. So IntOn runs what is queued even when interrupts
 * were on already. The Wimp's idle loop is OS_Mouse, INKEY, the time and
 * OS_IntOn, round again, all inside Wimp_Poll. The pointer's movement and
 * the keys come in there, as the IRQ breaks into the loop on a Pi. */
os_error *xos_int_on(void)
{
    ros_irq_on();
    if (!ros_irq_off_count())
        ros_background_run();
    return NULL;
}

os_error *xos_int_off(void)
{
    ros_irq_off();
    return NULL;
}

/* Read from the clock, so it is current even while ticks wait for a safe
 * point. MetroGnome catches up at the next one (ticker.h). */
os_error *xos_read_monotonic_time(uint32_t *cs)
{
    *cs = ros_monotonic_cs();
    return NULL;
}

/* OS_ReadMemMapInfo: 4K pages, and the machine's RAM in them.
 *
 * Everything that RISC OS hands out is counted against this number (the
 * free pool, dynarea.c). It is not an address-space limit. A task's slot and
 * a dynamic area are each their own mapping, paged in from the machine's
 * memory as they are touched. What the box actually uses is the sum of them.
 * That can be far more than one 32-bit address space holds.
 *
 * It is capped at 2 GB because RISC OS hands memory sizes about in signed
 * 32-bit words, and past that they read as negative. This affects the Task
 * Manager's free memory, Wimp_SlotSize and OS_ReadDynamicArea. rosgd.ram=<MB>
 * overrides the cap, for a machine with memory to spare and a user who
 * would rather have the memory than have those values read right. */
os_error *xos_read_mem_map_info(uint32_t *page_size, uint32_t *pages)
{
    long n = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    uint64_t bytes = n > 0 && size > 0 ? (uint64_t)n * (uint64_t)size : 0x10000000u;
    uint64_t most = 0x80000000u - 4096;
    const char *ram = ros_cmdline_value("rosgd.ram");
    if (ram) {
        uint64_t mb = strtoull(ram, NULL, 10);
        if (mb >= 16)
            most = mb << 20 > 0xFFFFF000u ? 0xFFFFF000u : mb << 20;
    }
    if (bytes > most)
        bytes = most;
    *page_size = 4096;
    *pages = (uint32_t)(bytes / 4096);
    return NULL;
}

/* Sends each character through WrchV, as OS_WriteC does, so that spooling
 * and redirection see them. */
os_error *xos_write_n(const uint8_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        os_error *e = xos_write_c(s[i]);
        if (e)
            return e;
    }
    return NULL;
}

