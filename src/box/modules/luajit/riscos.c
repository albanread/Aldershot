/* SPDX-License-Identifier: MIT
 * Copyright (c) 2011 Gavin Wraith (RiscLua's riscoslib.c)
 * Copyright (c) 2021 Lua.org, PUC-Rio (the Lua licence of RiscLua)
 * Copyright (c) 2026 Alban Read (this port to LuaJIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

/* riscos.c: the riscos library for the box's Lua -- the RISC OS face of
 * the Lua language, ported from RiscLua's riscoslib.c (Gavin Wraith,
 * MIT; RiscLua 8.6, over Lua 5.4) onto LuaJIT's 5.1-era API.
 *
 * The library is what it was:
 *
 *   riscos.dim(n) | riscos.dim(string)   a block of memory, with # giving
 *                                        its size -- what SWIs are fed with
 *   riscos.sys("OS_Something", ...)      a SWI call, by name or number:
 *                                        strings are copied to buffers and
 *                                        passed by address; the eight
 *                                        registers come back
 *   riscos.dir(path)                     an iterator: name, filetype
 *   riscos.filetype(path)                -1, &1000, &2000, or the type
 *   riscos.![adr] = v / riscos.![adr]    a word of memory
 *   riscos.?[adr] = v / riscos.?[adr]    a byte of memory
 *   riscos.$[adr] / riscos.$(adr, n)     a string at an address
 *
 * What changed for LuaJIT: lua_newuserdatauv (5.4) is lua_newuserdata,
 * luaL_newlib (5.2) is a table and luaL_register here, and the strings
 * for SWI calls are staged by the seam (riscos_sys.h) rather than kept
 * in buffers of the library's own.  The view addresses are carried at
 * the build's full width (the module is 64-bit C; the addresses Lua
 * scripts see are under 4 GB anyway, in the arena). */
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "riscos_sys.h"
#include <string.h>

#define hack(n) (void *)(uintptr_t)(n)

#define RISCLUA_VERSION "riscos 1.00 (the box's LuaJIT, from RiscLua 8.6)"

#define BLOCK "block"

/* do-while: it stands in single-statement ifs, as "if (bad) riscos_exit(...);" */
#define riscos_exit(L,s)  do { lua_pushnil(L); lua_pushstring(L,s); return 2; } while (0)

static int metablock, blockref;

typedef struct dimblock {
  size_t size;
  char val[1];
} dimblock;

/* luaL_newlib, as LuaJIT's 5.1 API has it: a fresh table, the functions
 * registered into it, the table left on the stack */
static void risc_newlib (lua_State *L, const luaL_Reg *t)
{
  lua_newtable(L);
  luaL_register(L, NULL, t);
}

static int risc_blocksize (lua_State *L) {
  void *q = lua_touserdata(L,1);
  if ((q != NULL) && (lua_getmetatable(L,1))) {
    lua_rawgeti(L,LUA_REGISTRYINDEX,metablock);
    if (!lua_rawequal(L,-1,-2)) q = NULL;
    lua_pop(L,2);
  }
  if (q) lua_pushinteger(L,(lua_Integer)((dimblock *)q)->size);
  else lua_pushnil(L);  /* should never get here */
  return 1;
}

/* The !/?/$ views take an address as a Lua integer and use it at the
 * width the build runs at. */
static int risc_r_w (lua_State *L)
{
  lua_Integer n = luaL_checkinteger(L,2);
  n = *(int32_t *)hack(n);
  lua_pushinteger(L, n);
  return 1;
}

static int risc_r_b (lua_State *L)
{
  lua_Integer n = luaL_checkinteger(L,2);
  n = *(unsigned char *)hack(n);
  lua_pushinteger(L, n);
  return 1;
}

static int risc_w_w (lua_State *L)
{
  lua_Integer n = luaL_checkinteger(L,2);
  int32_t x = (int32_t)luaL_checkinteger(L,3);
  *(int32_t *)hack(n) = x;
  return 0;
}

static int risc_w_b (lua_State *L)
{
  lua_Integer n = luaL_checkinteger(L,2);
  int32_t x = (int32_t)luaL_checkinteger(L,3);
  *(unsigned char *)hack(n) = (unsigned char)x;
  return 0;
}

static int risc_r_s (lua_State *L)
{
  lua_pushstring(L,(char *)hack(luaL_checkinteger(L,2)));
  return 1;
}

static int risc_rx_s (lua_State *L)
{
  size_t n;
  char *p;
  const char *s = (char *)hack(luaL_checkinteger(L,2));
  if (lua_gettop(L) > 2)
    n = (size_t)luaL_checkinteger(L,3);
  else {
    for (p = (char *)s; *p > 31; p++);
    n = p - s;
  }
  lua_pushlstring(L,s,n);
  return 1;
}

static int risc_w_s (lua_State *L)
{
  size_t n;
  void *p = hack(luaL_checkinteger(L,2));
  const void *s = lua_tolstring(L,3,&n);
  if (s == NULL) luaL_error(L,"$: string required");
  memcpy(p,s,n);
  return 0;
}

static int __attribute__((const)) align(int n)
{
  return (n + 3) & (~3);
}

static int risc_dim (lua_State *L)
{
  int n;
  dimblock *p;
  size_t nbytes;
  size_t m;
  const char *s;
  switch (lua_type(L,1)) {
    case LUA_TNUMBER:
      n = (int)lua_tointeger(L,1);
      if (n < 0)
        luaL_error(L,"dim: negative argument");
      nbytes = align((int)(sizeof(dimblock) + (n-1)*sizeof(char)));
      p = (dimblock *)lua_newuserdata(L, nbytes);
      p->size = (size_t) n;
      break;
    case LUA_TSTRING:
      s = lua_tolstring(L,1,&m);
      nbytes = align((int)(sizeof(dimblock) + m*sizeof(char)));
      p = (dimblock *)lua_newuserdata(L, nbytes);
      p->size = m;
      memcpy(p->val,s,m);
      break;
    default:
      luaL_error(L,"dim: bad argument type");
      return 0;
  }
  lua_rawgeti(L,LUA_REGISTRYINDEX,metablock);
  /* stack: p, metablock */
  lua_setmetatable(L,-2);
  /* stack: p */
  lua_rawgeti(L,LUA_REGISTRYINDEX,blockref);
  /* stack:  p, riscos.block */
  lua_pushinteger(L,(lua_Integer)(uintptr_t)((dimblock *) p)->val);
  /* stack: p, riscos.block, adr */
  lua_pushvalue(L,-1);
  /* stack: p, riscos.block, adr, adr  */
  lua_pushvalue(L,-4);
  /* stack: p, riscos.block, adr, adr, p */
  lua_rawset(L,-4);  /* riscos.block[adr] = p */
  /* stack: p, riscos.block, adr */
  lua_pop(L,2);
  return 1;
}

static int callswi (lua_State *L) {
  uint32_t swinum;
  int stktop;
  const oserror *e;
  int i;

  stktop = lua_gettop(L) - 1;
  switch (lua_type(L,1)) {
   case LUA_TNUMBER:
     swinum = (uint32_t)lua_tointeger(L,1);
     break;
   case LUA_TSTRING:
     swinum = swi_str2num(lua_tostring(L,1));
     if (swinum == (uint32_t)-1)
       riscos_exit(L,"sys: SWI name not known");
     break;
   default:
     riscos_exit(L,"sys: bad swi type");
     break;
  }
  riscos_arg a[8];
  for (i = 0; i < 8; i++)
    a[i].is_string = 0, a[i].v = 0, a[i].s = 0, a[i].n = 0;
  for (i = 0;i < 8 && i < stktop;i++)
  {
   int j = i+2;
   switch (lua_type(L,j)) {
    case LUA_TNUMBER:
     a[i].v = lua_tointeger(L,j);
     break;
    case LUA_TNIL:
     break;
    case LUA_TSTRING:
     a[i].is_string = 1;
     a[i].s = lua_tolstring(L,j,&a[i].n);
     if (a[i].n > 255) {
      riscos_exit(L,"sys: string argument too long"); break; }
     break;
    default:
     riscos_exit(L,"sys: bad arg type");
     break;
    }
  }
  int64_t out[8];
  if ((e = swi_args(swinum, a, stktop < 8 ? stktop : 8, out)) != 0) {
    FILE *df = fopen("/tmp/ljd.txt", "a"); if (df) { fprintf(df, "ERR num=%x msg=%s\n", swinum, e->errmsg); fclose(df); }
    riscos_exit(L,e->errmsg);
  }
  for (i = 0;i<8;i++)
    lua_pushinteger(L,out[i]);
  return 8;
 }

static int rdir_iter (lua_State *L);
#define RDIR_BUFLEN 256
static int rdir_buf[RDIR_BUFLEN/sizeof(int)];

static int rdir_read (lua_State *L) {
  lua_pushinteger(L,0); /* start */
  lua_pushstring(L,lua_tostring(L,1));
  lua_pushcclosure(L,rdir_iter,2);
  return 1;
}

static int rdir_iter (lua_State *L) {
  int n;
  int offset = (int) lua_tointeger(L,lua_upvalueindex(1));
  const char * dir = lua_tostring(L,lua_upvalueindex(2));
  n = rdir(dir,rdir_buf,offset);
  if (n == -1) {
    lua_pushnil(L);
    return 1;
  } else {
    lua_pushinteger(L, n);
    lua_replace(L, lua_upvalueindex(1));       /* new offset */
    lua_pushstring(L, (char *)(&rdir_buf[6])); /* name */
    lua_pushinteger(L, rdir_buf[5]);           /* filetype */
    return 2;
  }
}

static int r_filetype (lua_State *L) {
  lua_pushinteger(L, file_type(lua_tostring(L,1)));
  return 1;
}

static const struct luaL_Reg risc_mblock [] = {
   { "__len", risc_blocksize },
   { NULL, NULL}
};

static const struct luaL_Reg risc_meta_w [] = {
   { "__index", risc_r_w },
   { "__newindex", risc_w_w },
   { NULL, NULL}
};

static const struct luaL_Reg risc_meta_b [] = {
   { "__index", risc_r_b },
   { "__newindex", risc_w_b },
   { NULL, NULL}
};

static const luaL_Reg risc_meta_s [] = {
   { "__index", risc_r_s },
   { "__newindex", risc_w_s },
   { "__call", risc_rx_s },
   { NULL, NULL}
};

static const luaL_Reg riscoslib [] = {
   { "dim", risc_dim},
   { "sys", callswi},
   { "dir", rdir_read},
   { "filetype", r_filetype},
   {NULL,NULL}
};

int luaopen_riscos (lua_State *L) {
  risc_newlib(L, riscoslib);
  /* stack: riscos */
  lua_pushstring(L, RISCLUA_VERSION);  /* stack: riscos version */
  lua_setfield(L,-2,"version");        /* stack: riscos */
  lua_newtable(L); /* to be ! */
  risc_newlib(L,risc_meta_w);
  lua_setmetatable(L,-2);
  lua_setfield(L,-2,"!");
  lua_newtable(L); /* to be ? */
  risc_newlib(L,risc_meta_b);
  lua_setmetatable(L,-2);
  lua_setfield(L,-2,"?");
  lua_newtable(L); /* to be $ */
  risc_newlib(L,risc_meta_s);
  lua_setmetatable(L,-2);
  lua_setfield(L,-2,"$");
  lua_newtable(L); /* to be block */
  lua_pushvalue(L,-1);
  lua_setfield(L,-3,BLOCK);
  blockref = luaL_ref(L,LUA_REGISTRYINDEX);
  risc_newlib(L,risc_mblock);
  metablock = luaL_ref(L,LUA_REGISTRYINDEX);
  return 1;
}
