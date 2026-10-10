/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* files.c -- RISC OS files for the tools: *MD5 reads them, *Tftp reads and
 * writes them. They go through FileSwitch (OS_Find, OS_GBPB, OS_Args), so
 * a name is a RISC OS name, with its paths and filing systems, as the
 * tools' own fopen() took it. Data passes through a buffer in the RMA,
 * where the SWIs can see it.
 */
#include <string.h>

#include "inetres.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define CHUNK 65536

static os_error *call(struct ros_cpu *s, uint32_t swi)
{
    ros_swi(s, ROS_X_BIT | swi);
    return s->v ? ros_ptr(s->r[0]) : NULL;
}

os_error *inet_fopen(struct inet_file *f, const char *name, int write)
{
    f->handle = 0;
    f->buf = ros_rma_alloc(CHUNK);
    if (!f->buf)
        return ros_error(0x101, "No room in RMA");
    size_t n = strlen(name) + 1;
    if (n > CHUNK)
        n = CHUNK;
    memcpy(f->buf, name, n);
    f->buf[n - 1] = 0;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = write ? 0x8F : 0x4F;                   /* no path; errors, not handle 0 */
    s.r[1] = ros_addr(f->buf);
    os_error *e = call(&s, 0x0D);                   /* OS_Find */
    if (!e && !s.r[0])
        e = ros_error(0xD6, "File '%s' not found", name);
    if (e) {
        ros_rma_free(f->buf);
        f->buf = NULL;
        return e;
    }
    f->handle = s.r[0];
    return NULL;
}

long inet_fread(struct inet_file *f, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t want = n - done < CHUNK ? n - done : CHUNK;
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 4, s.r[1] = f->handle, s.r[2] = ros_addr(f->buf), s.r[3] = (uint32_t)want;
        if (call(&s, 0x0C))                         /* OS_GBPB 4: read here */
            return done ? (long)done : -1;
        size_t got = want - s.r[3];
        memcpy((uint8_t *)buf + done, f->buf, got);
        done += got;
        if (got < want)
            break;
    }
    return (long)done;
}

long inet_fwrite(struct inet_file *f, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t want = n - done < CHUNK ? n - done : CHUNK;
        memcpy(f->buf, (const uint8_t *)buf + done, want);
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 2, s.r[1] = f->handle, s.r[2] = ros_addr(f->buf), s.r[3] = (uint32_t)want;
        if (call(&s, 0x0C))                         /* OS_GBPB 2: write here */
            return -1;
        done += want - s.r[3];
        if (s.r[3])
            break;
    }
    return (long)done;
}

long inet_ftell(struct inet_file *f)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 0, s.r[1] = f->handle;
    return call(&s, 0x09) ? -1 : (long)s.r[2];      /* OS_Args 0 */
}

int inet_fseek(struct inet_file *f, long pos)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 1, s.r[1] = f->handle, s.r[2] = (uint32_t)pos;
    return call(&s, 0x09) ? -1 : 0;                 /* OS_Args 1 */
}

os_error *inet_fclose(struct inet_file *f)
{
    os_error *e = NULL;
    if (f->handle) {
        struct ros_cpu s;
        ros_cpu_enter(&s);
        s.r[0] = 0, s.r[1] = f->handle;
        e = call(&s, 0x0D);                         /* OS_Find 0 */
        f->handle = 0;
    }
    if (f->buf)
        ros_rma_free(f->buf);
    f->buf = NULL;
    return e;
}

long inet_fsize(const char *name)
{
    char *b = ros_rma_alloc(strlen(name) + 1);
    if (!b)
        return -1;
    strcpy(b, name);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = 17, s.r[1] = ros_addr(b);
    os_error *e = call(&s, 0x08);                   /* OS_File 17: read its catalogue */
    ros_rma_free(b);
    return e || s.r[0] != 1 ? -1 : (long)s.r[4];
}

int inet_readline(char *buf, size_t max)
{
    char *b = ros_rma_alloc(max + 1);
    if (!b)
        return -1;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(b), s.r[1] = (uint32_t)max - 1, s.r[2] = 32, s.r[3] = 255, s.r[4] = 0;
    os_error *e = call(&s, 0x7D);                   /* OS_ReadLine32 */
    int n = -1;
    if (!e && !s.c) {
        n = (int)s.r[1];
        memcpy(buf, b, (size_t)n);
        buf[n] = 0;
    } else if (!e) {
        inet_escape();                              /* acknowledge it */
    }
    ros_rma_free(b);
    return n;
}
