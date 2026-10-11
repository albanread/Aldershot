/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* luajit.h: the LuaJIT language module (modules/luajit/luajit.c). */
#ifndef ROSGD_LUAJIT_H
#define ROSGD_LUAJIT_H

#include "rosgd/module.h"

/* The module's error numbers */
#define LUAJIT_ERR_NOROOM    0x620    /* a SWI call could not stage its strings */
#define LUAJIT_ERR_NOTFOUND  0x621    /* the script was not found */
#define LUAJIT_ERR_LUA       0x622    /* the script failed: Lua's own message */

extern struct ros_module luajit_module;

#endif
