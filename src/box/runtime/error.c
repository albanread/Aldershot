/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* error.c: error blocks, and raising them. */
#include <stdarg.h>
#include <stdio.h>

#include "rosgd/arena.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/error.h"
#include "rosgd/platform.h"

static unsigned next_buffer;
static struct ros_handler *innermost;

os_error *ros_error(uint32_t errnum, const char *fmt, ...)
{
    /* In zero-page workspace, which compiled code can read. */
    uint32_t at = ROS_ZP_ERRBUFS + (next_buffer++ % ROS_ZP_ERRBUF_N) * sizeof(os_error);
    os_error *e = ros_ptr(at);
    va_list ap;
    va_start(ap, fmt);
    e->errnum = errnum;
    vsnprintf(e->errmess, sizeof e->errmess, fmt, ap);
    va_end(ap);
    return e;
}

void ros_handler_push(struct ros_handler *h)
{
    h->error = NULL;
    h->svc_sp = ros_svc_sp;
    h->call_depth = ros_call_depth;
    h->prev = innermost;
    innermost = h;
}

void ros_handler_pop(struct ros_handler *h)
{
    innermost = h->prev;
}

struct ros_handler *ros_handler_chain(struct ros_handler *chain)
{
    struct ros_handler *was = innermost;
    innermost = chain;
    return was;
}

static _Thread_local uint32_t error_pc;

uint32_t ros_error_pc_take(void)
{
    uint32_t pc = error_pc;
    error_pc = 0;
    return pc;
}

void ros_raise_at(const os_error *e, uint32_t pc)
{
    error_pc = pc;
    ros_raise(e);
}

void ros_raise(const os_error *e)
{
    struct ros_handler *h = innermost;
    if (!h)
        ros_env_raise(e);       /* no C handler: the program's error handler */
    error_pc = 0;               /* (a C handler has no buffer for it) */
    innermost = h->prev;
    h->error = e;
    ros_svc_sp = h->svc_sp;
    ros_call_depth = h->call_depth;     /* the SWIs it leaves are left */
    ros_resume_unwind(h);               /* and the frames */
    longjmp(h->jb, 1);
}
