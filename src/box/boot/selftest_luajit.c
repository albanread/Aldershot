/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* selftest_luajit.c -- LuaJIT, the Lua language module
 * (modules/luajit): *LuaJIT runs a script, print reaches the VDU, the
 * riscos library answers SWIs, directories and filetypes, -p parses
 * without running, and the failures are errors with Lua's own message.
 *
 * The scripts are on a HostFS disc of the test's own, "LuaTest", in a
 * directory made for it: in the box on the host share, hosted in
 * TMPDIR.  A .lua leaf arrives typed &18C through the typemap, which is
 * itself half the test.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileswitch.h"
#include "luamodule.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define WRCHV 0x03u

static uint32_t line;                       /* arena: a command */
static const os_error *last;
static char out[8192];
static unsigned outn;
static char root[512];

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1 && s->r[0] != 13)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return ROS_VECTOR_CLAIM;
}

/* OS_CLI: the error number or 0; what it printed in out */
static uint32_t cli(const char *cmd)
{
    strcpy(ros_ptr(line), cmd);
    outn = 0, out[0] = 0;
    ros_vector_claim_native(WRCHV, wrch, 0);
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line;
    ros_swi(&s, XOS_CLI);
    ros_vector_release_native(WRCHV, wrch, 0);
    last = s.v ? ros_ptr(s.r[0]) : NULL;
    return last ? last->errnum : 0;
}

static const char *last_error(void)
{
    return last ? last->errmess : "";
}

/* A variable's raw value, "unset" when there is none (no GSTrans) */
static const char *var(const char *name)
{
    static char v[512];
    strcpy(ros_ptr(line), name);
    uint32_t b = line + 512;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = line, s.r[1] = b, s.r[2] = 255, s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, XOS_ReadVarVal);
    if (s.v)
        return "unset";
    memcpy(v, ros_ptr(b), s.r[2]);
    v[s.r[2]] = 0;
    return v;
}

static void host_file(const char *leaf, const char *text)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, leaf);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            remove_tree(p);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

void ros_selftest_luajit(void)
{
    line = ros_addr(ros_rma_alloc(1024));

    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-luajit-XXXXXX", tmp);
    if (!mkdtemp(root)) {
        check(0, "LuaJIT -- no directory for the test disc", "no directory");
        return;
    }
    ros_hostfs_mount("LuaTest", root);

    host_file("hello,18c",
              "print('hello from LuaJIT')\n"
              "local riscos = require('riscos')\n"
              "print(riscos.version)\n"
              "for i = 1, #arg do io.write('arg ', i, ' ', arg[i], '\\n') end\n");
    host_file("sys,18c",
              "local riscos = require('riscos')\n"
              "riscos.sys('XOS_Write0', 'written by a SWI ')\n"
              "local t = { riscos.sys('OS_ReadSysInfo', 2) }\n"
              "print('page size ' .. t[1])\n");
    host_file("ftype,18c",
              "local riscos = require('riscos')\n"
              "print(string.format('%x', riscos.filetype('HostFS::LuaTest.$.hello')))\n");
    host_file("dir,18c",
              "local riscos = require('riscos')\n"
              "for name, ft in riscos.dir('HostFS::LuaTest.$') do\n"
              "  print(string.format('%s %x', name, ft))\n"
              "end\n");
    host_file("bad,18c", "error('boom from Lua')\n");
    host_file("syntax,18c", "if x then\n");

    /* The module, and the filetype's run action */
    struct ros_module *m = ros_module_first();
    while (m && strcmp(m->title, "LuaJIT") != 0)
        m = m->next;
    check(m && ros_module_version(m) == 0x10000,
          "the module is in the ROM, version 1.00", "%x",
          m ? (unsigned)ros_module_version(m) : 0u);
    int a1 = cli("LuaJIT") == 0 && strstr(out, "LuaJIT 2.1") != NULL;
    check(a1, "*LuaJIT with no script prints what it is", "%.40s", out);

    /* A script runs; print reaches the VDU; arguments arrive in arg */
    int r1 = cli("LuaJIT HostFS::LuaTest.$.hello one two") == 0 &&
             strstr(out, "hello from LuaJIT") != NULL &&
             strstr(out, "riscos 1.00") != NULL &&
             strstr(out, "arg 1 one") != NULL && strstr(out, "arg 2 two") != NULL;
    check(r1, "*LuaJIT runs a script: print through the VDU, arguments in arg", "%.60s", out);

    /* The riscos library: a SWI by name, registers back */
    int r2 = cli("LuaJIT HostFS::LuaTest.$.sys") == 0 &&
             strstr(out, "written by a SWI") != NULL;
    check(r2, "riscos.sys -- XOS_Write0 out, OS_ReadSysInfo's registers back (page size)",
          "%s / %.60s", last_error(), out);

    /* Directories and filetypes, as the typemap typed the .lua leaves */
    int r3 = cli("LuaJIT HostFS::LuaTest.$.ftype") == 0 &&
             strstr(out, "18c") != NULL;
    check(r3, "riscos.filetype of a typed leaf is &18C", "%.40s", out);
    int r4 = cli("LuaJIT HostFS::LuaTest.$.dir") == 0 &&
             strstr(out, "hello 18c") != NULL &&
             strstr(out, "bad 18c") != NULL;
    check(r4, "riscos.dir -- every leaf of the test disc with its filetype", "%.60s", out);

    /* The compiler: a hot loop, its sum right, jit.status as the build has it */
    host_file("jit,18c",
              "print('JIT ' .. (jit.status() and 'ON' or 'OFF'))\n"
              "local s = 0\n"
              "for i = 1, 5000000 do s = s + i end\n"
              "print('sum ' .. s)\n");
#if defined(__aarch64__)
    /* the arm64 builds have the compiler: the loop is traced and runs native */
    int j1 = cli("LuaJIT HostFS::LuaTest.$.jit") == 0 &&
             strstr(out, "JIT ON") != NULL &&
             strstr(out, "sum 12500002500000") != NULL;
    check(j1, "the compiler -- jit.status on, a five-million-iteration loop traced, its sum right",
          "%.50s", out);
#else
    int j1 = cli("LuaJIT HostFS::LuaTest.$.jit") == 0 &&
             strstr(out, "sum 12500002500000") != NULL;
    check(j1, "the hot loop, its sum right (the compiler as the build has it)", "%.50s", out);
#endif

    /* Failures are errors, with Lua's own message */
    int e1 = cli("LuaJIT HostFS::LuaTest.$.missing") == LUAJIT_ERR_NOTFOUND;
    check(e1, "a missing script is an error", "%s", last_error());
    int e2 = cli("LuaJIT HostFS::LuaTest.$.bad") == LUAJIT_ERR_LUA &&
             strstr(last_error(), "boom from Lua") != NULL;
    check(e2, "a script's error is the command's error", "%s", last_error());

    /* -p parses without running: good is silent, bad is the syntax error */
    int p2 = cli("LuaJIT -p HostFS::LuaTest.$.hello") == 0 &&
             strstr(out, "hello from LuaJIT") == NULL;
    check(p2, "*LuaJIT -p parses without running", "%.40s", out);
    int p3 = cli("LuaJIT -p HostFS::LuaTest.$.syntax") == LUAJIT_ERR_LUA &&
             strstr(last_error(), "syntax") != NULL;
    check(p3, "*LuaJIT -p reports a syntax error", "%s", last_error());

    /* The run action and the type name the module's init set */
    int v1 = strcmp(var("Alias$@RunType_18C"), "LuaJIT %*0") == 0;
    int v2 = strcmp(var("File$Type_18C"), "Lua") == 0;
    check(v1 && v2, "the module set the run action and the type name of &18C",
          "%s / %s", var("Alias$@RunType_18C"), var("File$Type_18C"));

    ros_hostfs_unmount("LuaTest");
    remove_tree(root);
    ros_rma_free(ros_ptr(line));
}
