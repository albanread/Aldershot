/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* luajit.c: LuaJIT, the box's Lua language (native module).
 *
 * *LuaJIT <script> runs a Lua script of filetype &18C (or any name it is
 * given), with the riscos library beside Lua's own: SWIs by name or
 * number, memory views, RISC OS directories and filetypes -- RiscLua's
 * library (Gavin Wraith), ported.  *LuaJIT -p <script> only parses.
 * Double-clicking a &18C file runs it: the module's init sets the type's
 * run action, as BoxTools does for Mojo's.
 *
 * LuaJIT 2.1, linked into /init (deps/build-luajit.sh): on the Apple
 * Silicon box and the hosted test the compiler is on, and its arm64
 * backend emits native code in the module's own 64-bit world; the Intel
 * box runs the interpreter until the x64 backend is switched on the
 * same way.  What a script prints goes through the VDU (WrCH), so it
 * lands on the console, in a TaskWindow, and in the self-test's
 * captured output alike. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#include "luamodule.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

int luaopen_riscos(lua_State *L);

/* ---- output: the VDU ------------------------------------------------- */

static os_error *write_s(const char *s)
{
    os_error *e = NULL;
    for (; *s && !e; s++)
        e = xos_write_c((uint8_t)*s);
    return e;
}

/* print, as Lua's own, but each character through OS_WriteC.  Lua 5.1's
 * tostring: strings and numbers as they are, other values their type's
 * name. */
static int lua_print(lua_State *L)
{
    static const char tab = '\t';
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1 && xos_write_c((uint8_t)tab))
            return luaL_error(L, "cannot write to the VDU");
        size_t l = 0;
        const char *s = lua_tolstring(L, i, &l);
        if (!s)
            s = lua_typename(L, lua_type(L, i)), l = strlen(s);
        for (size_t j = 0; j < l; j++)
            if (xos_write_c((uint8_t)s[j]))
                return luaL_error(L, "cannot write to the VDU");
    }
    xos_new_line();
    return 0;
}

/* io.write: the same, without the separators and the new line */
static int lua_write(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t l;
        const char *s = luaL_checklstring(L, i, &l);
        for (size_t j = 0; j < l; j++)
            if (xos_write_c((uint8_t)s[j]))
                return luaL_error(L, "cannot write to the VDU");
    }
    return 0;
}

/* ---- the interpreter -------------------------------------------------- */

static lua_State *new_state(void)
{
    lua_State *L = luaL_newstate();
    if (!L)
        return NULL;
    luaL_openlibs(L);
    /* require("riscos") is preloaded: the box has no dynamic loader */
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_riscos);
    lua_setfield(L, -2, "riscos");
    lua_pop(L, 2);
    lua_pushcfunction(L, lua_print);
    lua_setglobal(L, "print");
    lua_getglobal(L, "io");
    lua_pushcfunction(L, lua_write);
    lua_setfield(L, -2, "write");
    lua_pop(L, 1);
    return L;
}

/* A script's bytes and length, or NULL with errno-like why: 1 not
 * found, 2 no room.  Through OS_File, as the box's own code reads one */
static char *load_script(const char *name, uint32_t *len, int *why)
{
    void *kn = 0, *kb = 0;
    char *buf = NULL;
    size_t nn = strlen(name) + 1;
    char *n = ros_rma_alloc((uint32_t)nn);
    if (!n) {
        *why = 2;
        return NULL;
    }
    memcpy(n, name, nn);
    kn = n;
    uint32_t r[8] = { 23, ros_addr(n) };
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8);
    ros_swi(&s, XOS_File);
    memcpy(r, s.r, 32);
    if (s.v || r[0] == 0 || r[4] == 0) {
        *why = 1;
        goto out;
    }
    buf = malloc(r[4]);
    kb = ros_rma_alloc(r[4]);
    if (!buf || !kb) {
        free(buf);
        buf = NULL;
        *why = 2;
        goto out;
    }
    ros_cpu_enter(&s);
    s.r[0] = 16, s.r[1] = ros_addr(n), s.r[2] = ros_addr(kb), s.r[3] = 0;
    ros_swi(&s, XOS_File);
    if (s.v) {
        free(buf);
        buf = NULL;
        *why = 1;
        goto out;
    }
    memcpy(buf, kb, s.r[4]);
    *len = s.r[4];
out:
    if (kb)
        ros_rma_free(kb);
    ros_rma_free(kn);
    return buf;
}

/* ---- the command ------------------------------------------------------ */

static os_error *cmd_luajit(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char line[1024];
    size_t i = 0;
    for (uint32_t c; i + 1 < sizeof line && (c = ros_ld8(tail + (uint32_t)i)) >= ' '; i++)
        line[i] = (char)c;
    line[i] = 0;

    char *p = line;
    while (*p == ' ')
        p++;
    if (!*p) {
        write_s(LUAJIT_VERSION " -- the box's Lua.  ");
        lua_State *L = luaL_newstate();
        if (L) {
            luaL_openlibs(L);
            lua_getglobal(L, "jit");
            lua_getfield(L, -1, "status");
            lua_call(L, 0, 1);
            write_s(lua_toboolean(L, -1)
                        ? "The compiler is on: hot traces of Lua run as native code.\r"
                        : "The interpreter only: this build has no compiler.\r");
            lua_close(L);
        }
        write_s("Scripts are filetype &18C; the riscos library answers SWIs,\r"
                "directories and filetypes.  Syntax: *LuaJIT [-p] <script> [<arguments>]\r");
        return NULL;
    }

    int parse_only = 0;
    if (p[0] == '-' && p[1] == 'p' && p[2] == ' ') {
        parse_only = 1;
        p += 3;
        while (*p == ' ')
            p++;
    }
    char *name = p, *args = p;
    while (*args && *args != ' ')
        args++;
    if (*args)
        *args++ = 0;
    while (*args == ' ')
        args++;

    uint32_t len;
    int why;
    char *buf = load_script(name, &len, &why);
    if (!buf)
        return why == 2
            ? ros_error(LUAJIT_ERR_NOROOM, "Not enough memory to read %s", name)
            : ros_error(LUAJIT_ERR_NOTFOUND, "LuaJIT: %s not found", name);

    lua_State *L = new_state();
    if (!L) {
        free(buf);
        return ros_error(LUAJIT_ERR_NOROOM, "Not enough memory for the interpreter");
    }
    /* the command line as the script sees it: arg[0] the script, then */
    lua_createtable(L, 8, 0);
    lua_pushstring(L, name);
    lua_rawseti(L, -2, 0);
    int na = 0;
    char *w = args;
    while (*w) {
        while (*w == ' ')
            w++;
        if (!*w)
            break;
        char *e = w;
        while (*e && *e != ' ')
            e++;
        if (*e)
            *e++ = 0;
        lua_pushstring(L, w);
        lua_rawseti(L, -2, ++na);
        w = e;
    }
    lua_setglobal(L, "arg");

    int status = luaL_loadbuffer(L, buf, len, name);
    if (status == 0 && !parse_only)
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    free(buf);
    os_error *err = NULL;
    if (status != 0) {
        const char *msg = lua_tostring(L, -1);
        if (!msg)
            msg = "(error object is not a string)";
        err = ros_error(LUAJIT_ERR_LUA, "LuaJIT: %s", msg);
    }
    lua_close(L);
    return err;
}

static const struct ros_command commands[] = {
    { "LuaJIT", ROS_CMD_INFO(0, 255, 0, 0),
      "Syntax:\t*\x1B\x00 [-p] <script> [<arguments>]",
      "*LuaJIT runs a Lua script (filetype &18C) under LuaJIT, with the\r"
      "riscos library beside Lua's own: SWIs by name or number (riscos.sys),\r"
      "memory blocks and views (riscos.dim, ! ? $), directories and filetypes\r"
      "(riscos.dir, riscos.filetype). With -p it parses the script without\r"
      "running it. What the script prints goes to the VDU.\r",
      cmd_luajit },
    { 0 },
};

/* ---- filetype &18C's run action, as BoxTools does for Mojo's ---------- */

static int varval(const char *name, char *out, size_t max)
{
    size_t nn = strlen(name) + 1;
    char *b = ros_rma_alloc(256);
    char *n = ros_rma_alloc((uint32_t)nn);
    if (!b || !n) {
        if (b) ros_rma_free(b);
        if (n) ros_rma_free(n);
        return 0;
    }
    memcpy(n, name, nn);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(n), c.r[1] = ros_addr(b), c.r[2] = 255, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    int got = !c.v && c.r[2] < 1023;
    if (got)
        snprintf(out, max, "%.*s", (int)c.r[2], b);
    ros_rma_free(b);
    ros_rma_free(n);
    return got;
}

static void default_var(const char *name, const char *value)
{
    char have[8];
    if (varval(name, have, sizeof have))
        return;
    size_t nn = strlen(name), vn = strlen(value);
    char *b = ros_rma_alloc((uint32_t)(nn + vn + 2));
    if (!b)
        return;
    memcpy(b, name, nn + 1);
    memcpy(b + nn + 1, value, vn + 1);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b + nn + 1), c.r[2] = (uint32_t)vn, c.r[3] = 0, c.r[4] = 0;
    ros_swi(&c, XOS_SetVarVal);
    ros_rma_free(b);
}

static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    default_var("Alias$@RunType_18C", "LuaJIT %*0");
    default_var("File$Type_18C", "Lua");
    return NULL;
}

struct ros_module luajit_module = {
    .title = "LuaJIT",
    /* The version parser reads the first number from column 16 on, with
     * the tab padding to a multiple of 8: the first field wants 7 or
     * more characters, as Machine's and Boot Commands' have */
    .help = "LuaJIT, the box's Lua\t1.00 (10 Oct 2026) ROSGD native",
    .init = init,
    .commands = commands,
};
