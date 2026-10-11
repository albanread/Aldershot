/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* sys_box.c: the riscos library's seam (riscos_sys.h), answered from the
 * box's own SWI dispatch -- what RiscLua's src/s/sys did in ARM
 * assembler, as calls through ros_swi.  Everything a SWI sees lives in
 * the arena (the RMA) for exactly the length of the call: nothing is
 * kept between calls, so the library needs no workspace. */
#include "riscos_sys.h"
#include "luamodule.h"

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#include <string.h>

/* A SWI with registers: the error, or NULL; out the registers (the
 * shape BootCmds uses).  os_error and the seam's oserror are laid out
 * the same, so the block the dispatch returns is the seam's. */
static os_error *swi(uint32_t number, uint32_t *r, int n)
{
    struct ros_cpu c;
    ros_cpu_enter(&c);
    memcpy(c.r, r, (size_t)n * 4);
    ros_swi(&c, number);
    memcpy(r, c.r, 8 * 4);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* A name in the arena, for a call: its address, or 0 with *kept set to
 * free again on the way out */
static uint32_t arena_name(const char *s, void **kept)
{
    size_t n = strlen(s) + 1;
    char *b = ros_rma_alloc((uint32_t)n);
    if (!b)
        return 0;
    memcpy(b, s, n);
    *kept = b;
    return ros_addr(b);
}

/* riscos.sys: the registers out are sign-extended, as RiscLua's were
 * through int, so -1 is -1 in Lua and not 4294967295 */
oserror *swi_args(uint32_t swinum, const riscos_arg *a, int nargs,
                  int64_t out[8])
{
    uint32_t r[8];
    void *pool[8];
    unsigned np = 0;
    os_error *e = NULL;

    for (int i = 0; i < 8; i++) {
        r[i] = 0;
        if (i < nargs && a[i].is_string) {
            char *b = ros_rma_alloc((uint32_t)a[i].n + 1);
            if (!b) {
                e = ros_error(LUAJIT_ERR_NOROOM, "Not enough memory to call a SWI");
                break;
            }
            memcpy(b, a[i].s, a[i].n);
            b[a[i].n] = 0;
            pool[np++] = b;
            r[i] = ros_addr(b);
        } else if (i < nargs) {
            r[i] = (uint32_t)(uint64_t)a[i].v;
        }
    }
    if (!e)
        e = swi(swinum | ROS_X_BIT, r, 8);
    for (unsigned i = 0; i < np; i++)
        ros_rma_free(pool[i]);
    if (out)
        for (int i = 0; i < 8; i++)
            out[i] = (int64_t)(int32_t)r[i];
    return (oserror *)e;
}

/* OS_GBPB 12: one item at a time, the context an opaque cookie -- which
 * is what the filing system itself keeps, so nothing is held here
 * between calls either */
int rdir(const char *dir, int *buf, int offset)
{
    void *kn = 0, *rb = 0;
    uint32_t n = arena_name(dir, &kn);
    int result = -1;
    if (n) {
        rb = ros_rma_alloc(256);
        if (rb) {
            uint32_t r[8] = { 12, n, ros_addr(rb), 1, (uint32_t)offset, 256, 0 };
            os_error *e = swi(XOS_GBPB, r, 7);
            if (!e && r[3] == 1) {          /* one item read */
                memcpy(buf, rb, 24);
                strncpy((char *)buf + 24, (char *)rb + 24, 231);
                ((char *)buf)[24 + 231] = 0;
                result = (int)r[4];         /* the context to continue with */
            }
        }
    }
    if (rb)
        ros_rma_free(rb);
    if (kn)
        ros_rma_free(kn);
    return result;
}

/* OS_File 23: -1 when it is not there, else the filetype word (&1000
 * for a directory, &2000 for an application) */
int file_type(const char *name)
{
    void *kn = 0;
    uint32_t n = arena_name(name, &kn);
    int result = -1;
    if (n) {
        uint32_t r[8] = { 23, n };
        os_error *e = swi(XOS_File, r, 2);
        if (!e && r[0] != 0)
            result = (int)r[6];
    }
    if (kn)
        ros_rma_free(kn);
    return result;
}

/* OS_SWINumberFromString */
unsigned int swi_str2num(const char *s)
{
    void *kn = 0;
    uint32_t n = arena_name(s, &kn);
    unsigned int result = (unsigned int)-1;
    if (n) {
        uint32_t r[8] = { 0, n };
        os_error *e = swi(XOS_SWINumberFromString, r, 2);
        if (!e)
            result = r[0];
    }
    if (kn)
        ros_rma_free(kn);
    return result;
}
