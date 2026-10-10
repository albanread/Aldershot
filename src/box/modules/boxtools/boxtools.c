/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* boxtools.c: BoxTools, a native module for writing software in the box.
 *
 *   *CC <source>... [-o <program>] [tcc options...]
 *        This is the hosted form. It compiles and links C with the Mac's
 *        tcc into a program that the runtime's Mac runs. The boxes' *CC is
 *        clang, described below. The sources are RISC OS names. The program
 *        for c.hello is hello, beside the c directory. That is, the last
 *        "c" of the path is left out, and the program for hello/c is
 *        hello. A source spelt in neither way needs -o. A program that
 *        would overwrite one of its own sources is refused.
 *        The program is typed ELF, &E1F. BoxTools makes the run action of
 *        an ELF file *RunBox (Alias$@RunType_E1F, unless already set). So
 *        "hello", *Run hello, or a double-click runs it, as any RISC OS
 *        program runs.
 *   *RunBox <program> [arguments...]
 *        runs the program. The arguments are the program's own, as typed.
 *        Only the program's name is a RISC OS name. Its exit status is
 *        Sys$ReturnCode.
 *   *RosAsm <source> [rosasm options...]
 *        runs BOX's assembler on RISC OS names. The names are
 *        the source and the values of -o, -I (-i), --map, --swis and
 *        --swi-regs. It works where the runtime is hosted. The box has no
 *        rosasm, because its C is for the ROM.
 *   *RosBas <source> [-o <program>] [-L <directory>]... [--quirk <name>]...
 *        compiles a BBC BASIC V program into a RISC OS application. rosbas
 *        makes C from the program. The box's clang (x32-tcc when hosted)
 *        compiles the C. roscc links the result with the rosbas runtime
 *        (librb.a) against the ROM's SharedCLibrary. The application is an
 *        x32 or A64X32 ELF image typed &FF8. It runs by its name in the box,
 *        as any application does. The program for bas.hello is hello, as
 *        *CC names a program, and the program for hello/bas is hello.
 *        Another name needs -o. -L names where LIBRARY looks after the
 *        program's directory. The quirks are rosbas's (--quirks all).
 *        Hosted, it makes the same image, for the box. The hosted runtime
 *        runs no 32-bit code.
 *   *RosBasRun <source> [-o <image>] [-L <directory>]... [--quirk <name>]... [--no-run]
 *        compiles a BBC BASIC V program and runs it at once, as *Run runs
 *        an application. In the Apple Silicon box, rosbas's JIT (rosbas
 *        run --target a64x32) compiles the program to an A64X32 image in
 *        memory and writes it. No C, clang or roscc program is used. The
 *        image is a scrap file. Its name is Wimp$ScrapDir's RosBasRun, or
 *        else the source's name and _jit. The scrap file is removed after
 *        the run if the run comes back, which it does not in a task
 *        window. If -o is given, the image is -o's file and is kept.
 *        --no-run only makes the image for -o. The Intel box has no JIT.
 *        There the command runs *RosBas to make the image, and then runs
 *        the image. Hosted, the command is refused.
 *   *Mojo <source> [-o <program>]
 *        compiles a Mojo program into a RISC OS application. mojo (the
 *        box's /usr/bin/mojo, Mojo 1.1) compiles the program to an object
 *        for the box's ABI. That ABI is x32, or A64X32 with x18 reserved.
 *        For A64X32 the compile goes by way of LLVM IR, which roscc mojo-ir
 *        fits for A64X32 and the box's clang compiles. The compile is made
 *        against the standard library and the riscos and demos packages
 *        (/usr/lib/mojo/lib/mojo and /usr/lib/mojo/pkg). roscc links the
 *        object with the Mojo runtime (/usr/lib/mojo/rt_x32 or
 *        rt_a64x32). The runtime has the start code, rostrt, the SWI gate
 *        shims (os and wimp as objects, the other chunks' as an archive)
 *        and the maths functions as an archive. The application is typed
 *        &FF8 and runs by its name. The program for mojo.hello is hello, as
 *        *CC names a program, and the program for hello/mojo is hello (a
 *        host file hello.mojo). Another name needs -o. Mojo source is typed
 *        &A6E, Mojo (File$Type_A6E, unless already set). A host file's
 *        .mojo says so (HostFS's typemap). Mojo$Exe, if set, names another
 *        compiler.
 *        --kernel makes a Worker kernel file instead (roscc link
 *        --kernel, typed Data). The kernel is the source's `@export def
 *        kernel(args, results, cancel) -> UInt32`. It calls nothing and has
 *        no statics. It is for Worker_LoadKernel
 *        (riscos.workers.load_kernel).
 *
 * Both boxes have clang. It is the patched clang that builds A64X32 on the
 * host, with lld, as one program. The Intel box has the same for x86-64,
 * and it replaced that box's tcc (deps/build-clang-box.sh [--arch
 * x86_64]). On the boxes, *CC does what the user asked for: "cc --linux is
 * meant to create a linux app otherwise create a riscos app".
 *   *CC [--linux] <source>... [-o <program>] [clang options...]
 *        By default it makes a RISC OS application, typed &FF8, run by its
 *        name in its slot. Each source is compiled A64X32 or x32 by clang
 *        (its default target, whose config file is A64X32's flag table)
 *        against RISC OS's C headers. roscc then links the objects
 *        (link --clib) against the ROM's SharedCLibrary. With --linux, it
 *        makes a Linux program, typed &E1F, run by *RunBox. For this, clang
 *        compiles for aarch64- or x86_64-unknown-linux-musl (LP64, because
 *        Linux has no ILP32 ABI on arm64). The program is static and is
 *        built against musl and linked by ld.lld. The box has no dynamic
 *        loader.
 *   *RosBas compiles rosbas's C with the same clang against the box's
 *        kit, and roscc links it with --a64x32-rt or --x32-rt.
 *
 * The programs are clang or tcc, rosasm, and a program that *CC --linux
 * made. They are Linux programs (or, hosted, the Mac's programs). Each
 * runs as the PTY module runs ssh. It is a child on a pseudo-terminal,
 * drawn with VDU codes in the text window, with the RISC OS keyboard as its
 * input (pty_terminal). Its crash or its exit() is its own.
 *
 * Clang$Exe, Tcc$Exe (hosted) and RosAsm$Exe, if set, name the programs
 * (as Linux paths). Otherwise the box's /usr/bin/clang is used or, hosted,
 * the builds that the Makefile names. *RosBas's programs and files are the
 * box's /usr/bin/rosbas, /usr/bin/clang, /usr/bin/roscc, /usr/lib/rosbas
 * (the runtime), /usr/lib/capps/inc/C (RISC OS's C headers) and
 * /usr/lib/roscc/x32 (an application's start and the library's stubs).
 * Hosted, they are the builds' equivalents. Errors are BoxTools' own,
 * &C0120 + n, in the application range as PTY's are.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boxtools.h"
#include "fileswitch.h"
#include "pty.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"

#define ERR_BOX 0xC0120u
#define MAXW 48
#define TYPE_ELF 0xE1Fu
#define TYPE_APP 0xFF8u
#define TYPE_DATA 0xFFDu

#ifndef BOXTOOLS_TCC
#define BOXTOOLS_TCC "/usr/bin/tcc"
#endif
#ifndef BOXTOOLS_ROSASM
#define BOXTOOLS_ROSASM ""
#endif
/* *RosBas: the box's, unless the hosted build names its own */
#ifndef BOXTOOLS_ROSBAS
#define BOXTOOLS_ROSBAS "/usr/bin/rosbas"
#endif
#ifndef BOXTOOLS_X32TCC
#define BOXTOOLS_X32TCC "/usr/bin/x32-tcc"
#endif
#ifndef BOXTOOLS_ROSCC
#define BOXTOOLS_ROSCC "/usr/bin/roscc"
#endif
#ifndef BOXTOOLS_ROSBAS_KIT
#define BOXTOOLS_ROSBAS_KIT "/usr/lib/rosbas"
#endif
#ifndef BOXTOOLS_CAPPS_INC
#define BOXTOOLS_CAPPS_INC "/usr/lib/capps/inc/C"
#endif
#ifndef BOXTOOLS_X32_RT
#define BOXTOOLS_X32_RT "/usr/lib/roscc/x32"
#endif
#ifndef BOXTOOLS_X32_LIBTCC
#define BOXTOOLS_X32_LIBTCC "/usr/lib/tcc/x32-libtcc1.a"
#endif
/* *Mojo: the compiler and its kit as the box's image has them (the
 * Makefile's MOJO_STAGE). These are the directory of the precompiled
 * standard library, and then the packages' directory, which holds the
 * riscos bindings and demos. */
#ifndef BOXTOOLS_MOJO
#define BOXTOOLS_MOJO "/usr/bin/mojo"
#endif
#ifndef BOXTOOLS_MOJO_STD
#define BOXTOOLS_MOJO_STD "/usr/lib/mojo/lib/mojo"
#endif
#ifndef BOXTOOLS_MOJO_PKG
#define BOXTOOLS_MOJO_PKG "/usr/lib/mojo/pkg"
#endif

/* Both boxes use clang for *CC and *RosBas. The Intel box's clang replaces
 * its tcc and is built by the same recipe for x86_64
 * (deps/build-clang-box.sh --arch x86_64). The hosted build, on either Mac,
 * keeps the host's tcc. */
#if !defined(ROS_ARENA_HOSTED)
#define BOXTOOLS_CLANG_BOX 1
#define BOXTOOLS_CLANG "/usr/bin/clang"
#if defined(__aarch64__)
#define BOXTOOLS_LINUX_TARGET "--target=aarch64-unknown-linux-musl"
#define BOXTOOLS_CLANG_RT_FLAG "--a64x32-rt"
#define BOXTOOLS_CLANG_RT "/usr/lib/roscc/a64x32"
#define BOXTOOLS_ABI "A64X32"            /* what *Help says it makes (#177) */
#else
#define BOXTOOLS_LINUX_TARGET "--target=x86_64-unknown-linux-musl"
#define BOXTOOLS_CLANG_RT_FLAG "--x32-rt"
#define BOXTOOLS_CLANG_RT "/usr/lib/roscc/x32"
#define BOXTOOLS_ABI "x32"
#endif
#define BOXTOOLS_A64X32_RT "/usr/lib/roscc/a64x32"
#define CC_WHAT "clang"
#define CC_SYNTAX "CC [--linux] <source>... [-o <program>] [clang options...]"
#define CC_HELP "*CC compiles and links C with clang: a RISC OS application (" BOXTOOLS_ABI ", roscc) " \
                "or, with --linux, a Linux program the box runs: c.hello's is hello."
#define ROSBAS_HELP "*RosBas compiles a BBC BASIC V program into a RISC OS application (rosbas, clang, " \
                    "roscc): bas.hello's is hello."
#define MOJO_WHAT BOXTOOLS_ABI
#if defined(__aarch64__)
#define ROSBASRUN_HELP "*RosBasRun compiles a BBC BASIC V program with rosbas's JIT to an A64X32 image in " \
                       "memory, with no C, and runs it at once."
#else
#define ROSBASRUN_HELP "*RosBasRun compiles a BBC BASIC V program with *RosBas to a scrap application and " \
                       "runs it at once."
#endif
#else
#define BOXTOOLS_CLANG_BOX 0
#define CC_WHAT "tcc"
#define CC_SYNTAX "CC <source>... [-o <program>] [tcc options...]"
#define CC_HELP "*CC compiles and links C with tcc into a program the box runs: c.hello's is hello."
#define MOJO_WHAT "x32"
#define ROSBAS_HELP "*RosBas compiles a BBC BASIC V program into a RISC OS application (rosbas, x32-tcc, " \
                    "roscc): bas.hello's is hello."
#define ROSBASRUN_HELP "*RosBasRun compiles and runs a BBC BASIC V program in the box; hosted it is refused."
#endif

/* ---- words, names, variables ------------------------------------------------ */

struct words {
    char buf[1100];
    char *w[MAXW + 1];
    int n;
};

/* Splits the command tail into words. A "quoted" word is kept whole. */
static os_error *split(const char *tool, uint32_t tail, struct words *ws)
{
    const char *t = ros_ptr(tail);
    size_t k = 0;
    ws->n = 0;
    while ((unsigned char)*t >= ' ') {
        while (*t == ' ')
            t++;
        if ((unsigned char)*t < ' ')
            break;
        if (ws->n == MAXW || k >= sizeof ws->buf - 2)
            return ros_error(ERR_BOX, "%s: too many words", tool);
        ws->w[ws->n++] = ws->buf + k;
        int quoted = *t == '"';
        if (quoted)
            t++;
        while ((unsigned char)*t >= ' ' && (quoted ? *t != '"' : *t != ' ')) {
            if (k >= sizeof ws->buf - 2)
                return ros_error(ERR_BOX, "%s: the command line is too long", tool);
            ws->buf[k++] = *t++;
        }
        if (quoted && *t == '"')
            t++;
        ws->buf[k++] = 0;
    }
    ws->w[ws->n] = NULL;
    return NULL;
}

/* Maps a RISC OS name (of a file that may not be there yet) to its Linux
 * path. A system variable in the name, <Obey$Dir> for example, is GSTrans'd
 * first, as FileSwitch does for a name given to it by SWI. */
static os_error *map(const char *name, char *out, size_t max)
{
    if (!strchr(name, '<'))
        return ros_hostfs_linux_path(name, out, max);
    size_t n = strlen(name);
    char *b = ros_rma_alloc((uint32_t)(n + 1 + 1024));
    if (!b)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(b, name, n + 1);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b + n + 1), c.r[2] = 1023u | 1u << 30 | 1u << 29;
    ros_swi(&c, XOS_GSTrans);
    os_error *e = c.v ? ros_ptr(c.r[0]) : c.c ? ros_error(ERR_BOX, "%s: the name is too long", name) : NULL;
    if (!e) {
        b[n + 1 + c.r[2]] = 0;
        e = ros_hostfs_linux_path(b + n + 1, out, max);
    }
    ros_rma_free(b);
    return e;
}

/* Reads a system variable, GSTrans'd (OS_ReadVarVal with R4 = 3). Returns 1 if it is set and not empty. */
static int varval(const char *name, char *out, size_t max)
{
    char *b = ros_rma_alloc(1100);
    if (!b)
        return 0;
    snprintf(b + 1024, 76, "%s", name);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b + 1024), c.r[1] = ros_addr(b), c.r[2] = 1023, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    int got = !c.v && c.r[2] < 1023;
    if (got)
        snprintf(out, max, "%.*s", (int)c.r[2], b);
    ros_rma_free(b);
    return got && out[0];
}

/* Sets a string variable, unless it is set already */
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

/* Sets a file's type (OS_File 18) */
static os_error *set_type(const char *name, uint32_t type)
{
    size_t n = strlen(name);
    char *b = ros_rma_alloc((uint32_t)n + 1);
    if (!b)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(b, name, n + 1);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 18, c.r[1] = ros_addr(b), c.r[2] = type;
    ros_swi(&c, XOS_File);
    ros_rma_free(b);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

/* Deletes a file if it is there (OS_File 6) */
static void remove_file(const char *name)
{
    size_t n = strlen(name);
    char *b = ros_rma_alloc((uint32_t)n + 1);
    if (!b)
        return;
    memcpy(b, name, n + 1);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = 6, c.r[1] = ros_addr(b);
    ros_swi(&c, XOS_File);
    ros_rma_free(b);
}

static void return_code(int rc)
{
    char *b = ros_rma_alloc(32);
    if (!b)
        return;
    memcpy(b, "Sys$ReturnCode", 15);
    uint32_t v = (uint32_t)rc;
    memcpy(b + 20, &v, 4);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b), c.r[1] = ros_addr(b + 20), c.r[2] = 4, c.r[3] = 0, c.r[4] = 1;
    ros_swi(&c, XOS_SetVarVal);
    ros_rma_free(b);
}

/* Makes a program's name from its source's name. The last component of the
 * source that is named dir (in either case), other than the leaf, is left
 * out. Or a "/dir" suffix is taken off. Returns 1 if either applied. */
static int program_name(const char *src, const char *dir, char *out, size_t max)
{
    size_t d = strlen(dir);
    const char *cut = NULL;
    for (const char *p = src; *p;) {
        const char *dot = strchr(p, '.');
        if (!dot)
            break;
        if ((size_t)(dot - p) == d && !strncasecmp(p, dir, d))
            cut = p;
        p = dot + 1;
    }
    if (cut) {
        snprintf(out, max, "%.*s%s", (int)(cut - src), src, cut + d + 1);
        return 1;
    }
    size_t n = strlen(src);
    if (n > d + 1 && src[n - d - 1] == '/' && !strcasecmp(src + n - d, dir)) {
        snprintf(out, max, "%.*s", (int)(n - d - 1), src);
        return 1;
    }
    return 0;
}

int boxtools_program_name(const char *src, char *out, size_t max)
{
    return program_name(src, "c", out, max);
}

/* ---- running a program ----------------------------------------------------------- */

struct line {
    char s[4096];
    size_t n;
};

static os_error *add(struct line *l, const char *tool, const char *word)
{
    if (strchr(word, '"'))
        return ros_error(ERR_BOX, "%s: a word cannot hold a \" (%s)", tool, word);
    int space = strchr(word, ' ') != NULL || !*word;
    int w = snprintf(l->s + l->n, sizeof l->s - l->n, space ? "%s\"%s\"" : "%s%s", l->n ? " " : "", word);
    if (w < 0 || (size_t)w >= sizeof l->s - l->n)
        return ros_error(ERR_BOX, "%s: the command line is too long", tool);
    l->n += (size_t)w;
    return NULL;
}

/* Runs a program on the PTY module's terminal. Returns an error if the
 * program could not run or a signal ended it. Otherwise its exit status is
 * stored in *status. Each line of the output that contains drop is left
 * out. */
static os_error *run_dropping(const char *tool, struct line *l, const char *drop, int *status)
{
    int st = 0;
    os_error *e = pty_terminal_dropping(l->s, drop, &st);
    if (e)
        return ros_error(ERR_BOX + 1, "%s: %s", tool, e->errmess);
    *status = st;
    return_code(st);
    return NULL;
}

static os_error *run(const char *tool, struct line *l, int *status)
{
    int st = 0;
    os_error *e = pty_terminal(l->s, &st);
    if (e)
        return ros_error(ERR_BOX + 1, "%s: %s", tool, e->errmess);
    *status = st;
    return_code(st);
    return NULL;
}

/* ---- *CC ----------------------------------------------------------------------- */

/* Says whether an option takes the next word as its value. It is used for
 * tcc's options and for clang's. *path is set to 1 for those whose value
 * names a file or directory, and to 0 for the others. */
static int takes_value(const char *o, int *path)
{
    static const char *const files[] = { "-o", "-I", "-L", "-include", "-isystem", NULL };
    static const char *const other[] = { "-D", "-U", "-l", "-x", NULL };
    for (int i = 0; files[i]; i++)
        if (!strcmp(o, files[i]))
            return *path = 1, 1;
    for (int i = 0; other[i]; i++)
        if (!strcmp(o, other[i]))
            return *path = 0, 1;
    return 0;
}

#if BOXTOOLS_CLANG_BOX
/* The boxes' *CC. It is used on both boxes, so the default target is
 * A64X32 on the Apple Silicon box and x32 on the Intel box. Options pass
 * to clang with their file values mapped, as tcc's do. Each source is
 * compiled as C (-x c). By default each source is compiled to an object
 * beside the program (<program>.cc<n>.o, removed afterwards). The compile
 * uses -c, clang's default target and config, and RISC OS's headers after
 * the program's own -I (as -isystem). roscc then links the objects
 * against the ROM's SharedCLibrary (link --clib, with the start code and
 * stubs for the ABI). The result is an application, &FF8. With --linux,
 * one clang run compiles and links a static Linux program, &E1F. Its
 * target is aarch64-unknown-linux-musl, or x86_64 on the Intel box. Its
 * config file selects musl, compiler-rt's builtins and ld.lld. */
static os_error *cc_clang(uint32_t tail)
{
    static struct words ws;
    os_error *e = split("CC", tail, &ws);
    if (e)
        return e;
    static char mapped[MAXW][1024], out[1024], objs[MAXW][1100];
    static const char *srcs[MAXW], *src_paths[MAXW];
    static const char *opts[2 * MAXW];         /* the options, as clang gets them */
    int nsrc = 0, nm = 0, nopt = 0, linux_prog = 0;
    const char *out_name = NULL;
    char clang[1024];
    if (!varval("Clang$Exe", clang, sizeof clang))
        snprintf(clang, sizeof clang, "%s", BOXTOOLS_CLANG);
    const char *libopt = NULL;                 /* -l or -L, which only --linux allows */
    for (int i = 0; i < ws.n; i++) {
        const char *w = ws.w[i];
        int path = 0;
        if (!strncmp(w, "-l", 2) || !strncmp(w, "-L", 2))
            libopt = w;
        if (!strcmp(w, "--linux")) {
            linux_prog = 1;
        } else if (w[0] == '-' && takes_value(w, &path)) {
            if (i + 1 >= ws.n)
                return ros_error(ERR_BOX, "CC: %s needs a value", w);
            const char *v = ws.w[++i];
            if (!strcmp(w, "-o")) {
                out_name = v;
                continue;
            }
            if (path) {
                if ((e = map(v, mapped[nm], sizeof mapped[nm])))
                    return e;
                v = mapped[nm++];
            }
            opts[nopt++] = w, opts[nopt++] = v;
        } else if (w[0] == '-' && (w[1] == 'I' || w[1] == 'L') && w[2]) {
            if ((e = map(w + 2, mapped[nm], sizeof mapped[nm])))
                return e;
            opts[nopt++] = w[1] == 'I' ? "-I" : "-L", opts[nopt++] = mapped[nm++];
        } else if (w[0] == '-') {
            if (!strncmp(w, "-B", 2) || !strncmp(w, "--target", 8) || !strcmp(w, "-target"))
                return ros_error(ERR_BOX, "CC: %s is the compiler's own (--linux makes a Linux program)", w);
            opts[nopt++] = w;
        } else {
            if ((e = map(w, mapped[nm], sizeof mapped[nm])))
                return e;
            src_paths[nsrc] = mapped[nm++];
            srcs[nsrc++] = w;
        }
    }
    if (!nsrc)
        return ros_error(ERR_BOX, "Syntax: *" CC_SYNTAX);
    if (!linux_prog && libopt)
        return ros_error(ERR_BOX, "CC: %s: a RISC OS application links against the ROM's "
                         "SharedCLibrary only (--linux makes a Linux program)", libopt);
    char prog[1024];
    if (!out_name) {
        if (!boxtools_program_name(srcs[0], prog, sizeof prog))
            return ros_error(ERR_BOX + 2, "CC: %s is not c.<name> or <name>/c: give -o", srcs[0]);
        out_name = prog;
    }
    if ((e = map(out_name, out, sizeof out)))
        return e;
    for (int i = 0; i < nm; i++)
        if (!strcmp(out, mapped[i]))
            return ros_error(ERR_BOX + 2, "CC: the program would be written over %s", out_name);
    static struct line l;
    int st;
    if (linux_prog) {
        /* 1. Compile and link, in one run. */
        l.n = 0, l.s[0] = 0;
        if ((e = add(&l, "CC", clang)) || (e = add(&l, "CC", BOXTOOLS_LINUX_TARGET)))
            return e;
        for (int i = 0; i < nopt; i++)
            if ((e = add(&l, "CC", opts[i])))
                return e;
        for (int k = 0; k < nsrc; k++)           /* the options came first, so no -x none is needed */
            if ((e = add(&l, "CC", "-x")) || (e = add(&l, "CC", "c")) || (e = add(&l, "CC", src_paths[k])))
                return e;
        if ((e = add(&l, "CC", "-o")) || (e = add(&l, "CC", out)))
            return e;
        if ((e = run("CC", &l, &st)))
            return e;
        if (st)
            return ros_error(ERR_BOX + 3, "CC: %s did not compile", srcs[0]);
        return set_type(out_name, TYPE_ELF);   /* runnable by name, because its run action is *RunBox */
    }
    /* 1. Compile each source to an object for the box's ABI. */
    int nobj = 0;
    for (int k = 0; k < nsrc; k++) {
        snprintf(objs[k], sizeof objs[k], "%s.cc%d.o", out, k);
        nobj = k + 1;
        l.n = 0, l.s[0] = 0;
        if ((e = add(&l, "CC", clang)))
            goto done;
        for (int i = 0; i < nopt; i++)
            if ((e = add(&l, "CC", opts[i])))
                goto done;
        if ((e = add(&l, "CC", "-isystem")) || (e = add(&l, "CC", BOXTOOLS_CAPPS_INC)) ||
            (e = add(&l, "CC", "-c")) || (e = add(&l, "CC", "-x")) || (e = add(&l, "CC", "c")) ||
            (e = add(&l, "CC", src_paths[k])) || (e = add(&l, "CC", "-o")) || (e = add(&l, "CC", objs[k])))
            goto done;
        if ((e = run("CC", &l, &st)))
            goto done;
        if (st) {
            e = ros_error(ERR_BOX + 3, "CC: %s did not compile", srcs[k]);
            goto done;
        }
    }
    /* 2. Link against the ROM's SharedCLibrary. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "CC", BOXTOOLS_ROSCC)) || (e = add(&l, "CC", "link")) || (e = add(&l, "CC", "--clib")) ||
        (e = add(&l, "CC", BOXTOOLS_CLANG_RT_FLAG)) || (e = add(&l, "CC", BOXTOOLS_CLANG_RT)) ||
        (e = add(&l, "CC", "-o")) || (e = add(&l, "CC", out)))
        goto done;
    for (int k = 0; k < nobj; k++)
        if ((e = add(&l, "CC", objs[k])))
            goto done;
    if ((e = run("CC", &l, &st)))
        goto done;
    if (st) {
        e = ros_error(ERR_BOX + 3, "CC: %s did not link (roscc)", srcs[0]);
        goto done;
    }
    e = set_type(out_name, TYPE_APP);          /* an application, run by *Run or by its name */
done:
    for (int k = 0; k < nobj; k++)
        unlink(objs[k]);
    return e;
}
#endif

static os_error *cmd_cc(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
#if BOXTOOLS_CLANG_BOX
    return cc_clang(tail);
#else
    static struct words ws;
    os_error *e = split("CC", tail, &ws);
    if (e)
        return e;
    static char mapped[MAXW][1024], out[1024];
    static const char *srcs[MAXW];
    int nsrc = 0, nm = 0;
    const char *out_name = NULL;
    char tcc[1024];
    if (!varval("Tcc$Exe", tcc, sizeof tcc))
        snprintf(tcc, sizeof tcc, "%s", BOXTOOLS_TCC);
    static struct line l;
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "CC", tcc)))
        return e;
#ifdef __linux__
    if ((e = add(&l, "CC", "-static")))
        return e;
#endif
    /* Pass the options as typed, with their file values mapped. Put each
     * source after -x c, because a RISC OS name has no .c for tcc to go by. */
    for (int i = 0; i < ws.n; i++) {
        const char *w = ws.w[i];
        int path = 0;
        if (w[0] == '-' && takes_value(w, &path)) {
            if (i + 1 >= ws.n)
                return ros_error(ERR_BOX, "CC: %s needs a value", w);
            const char *v = ws.w[++i];
            if (!strcmp(w, "-o")) {
                out_name = v;
                continue;
            }
            if ((e = add(&l, "CC", w)))
                return e;
            if (path) {
                if ((e = map(v, mapped[nm], sizeof mapped[nm])))
                    return e;
                v = mapped[nm++];
            }
            if ((e = add(&l, "CC", v)))
                return e;
        } else if (w[0] == '-' && (w[1] == 'I' || w[1] == 'L') && w[2]) {
            char opt[3] = { '-', w[1], 0 };
            if ((e = map(w + 2, mapped[nm], sizeof mapped[nm])))
                return e;
            if ((e = add(&l, "CC", opt)) || (e = add(&l, "CC", mapped[nm++])))
                return e;
        } else if (w[0] == '-') {
            if (!strncmp(w, "-B", 2))
                return ros_error(ERR_BOX, "CC: -B is tcc's own (Tcc$Exe names another tcc)");
            if ((e = add(&l, "CC", w)))
                return e;
        } else {
            srcs[nsrc++] = w;
            if ((e = map(w, mapped[nm], sizeof mapped[nm])))
                return e;
            if ((e = add(&l, "CC", "-x")) || (e = add(&l, "CC", "c")) ||
                (e = add(&l, "CC", mapped[nm++])) || (e = add(&l, "CC", "-x")) ||
                (e = add(&l, "CC", "none")))
                return e;
        }
    }
    if (!nsrc)
        return ros_error(ERR_BOX, "Syntax: *CC <source>... [-o <program>] [tcc options...]");
    char prog[1024];
    if (!out_name) {
        if (!boxtools_program_name(srcs[0], prog, sizeof prog))
            return ros_error(ERR_BOX + 2, "CC: %s is not c.<name> or <name>/c: give -o", srcs[0]);
        out_name = prog;
    }
    if ((e = map(out_name, out, sizeof out)))
        return e;
    for (int i = 0; i < nm; i++)
        if (!strcmp(out, mapped[i]))
            return ros_error(ERR_BOX + 2, "CC: the program would be written over %s", out_name);
    if ((e = add(&l, "CC", "-o")) || (e = add(&l, "CC", out)))
        return e;
    int st;
    if ((e = run("CC", &l, &st)))
        return e;
    if (st)
        return ros_error(ERR_BOX + 3, "CC: %s did not compile", srcs[0]);
    return set_type(out_name, TYPE_ELF);       /* runnable by name, because its run action is *RunBox */
#endif
}

/* ---- *RunBox ------------------------------------------------------------------------ */

static os_error *cmd_runbox(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    static struct words ws;
    os_error *e = split("RunBox", tail, &ws);
    if (e)
        return e;
    if (!ws.n)
        return ros_error(ERR_BOX, "Syntax: *RunBox <program> [arguments...]");
    char prog[1024];
    if ((e = map(ws.w[0], prog, sizeof prog)))
        return e;
    if (access(prog, X_OK) != 0)
        return ros_error(ERR_BOX + 4, "RunBox: %s is not a program", ws.w[0]);
    static struct line l;
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "RunBox", prog)))
        return e;
    for (int i = 1; i < ws.n; i++)
        if ((e = add(&l, "RunBox", ws.w[i])))
            return e;
    int st;
    return run("RunBox", &l, &st);             /* the status goes to Sys$ReturnCode */
}

/* ---- *RosAsm ------------------------------------------------------------------------ */

static os_error *cmd_rosasm(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    char exe[1024];
    if (!varval("RosAsm$Exe", exe, sizeof exe))
        snprintf(exe, sizeof exe, "%s", BOXTOOLS_ROSASM);
    if (!exe[0])
        return ros_error(ERR_BOX + 5, "RosAsm: there is no rosasm here (RosAsm$Exe names one)");
    static struct words ws;
    os_error *e = split("RosAsm", tail, &ws);
    if (e)
        return e;
    static const char *const files[] = { "-o", "-I", "-i", "--map", "--swis", "--swi-regs", NULL };
    static const char *const values[] = { "-PD", "--emit", "--rom-base", "--abi", "--clang", NULL };
    static char mapped[MAXW][1024];
    static struct line l;
    l.n = 0, l.s[0] = 0;
    int nm = 0, have_src = 0;
    if ((e = add(&l, "RosAsm", exe)))
        return e;
    for (int i = 0; i < ws.n; i++) {
        const char *w = ws.w[i];
        int file = 0, value = 0;
        for (int k = 0; files[k]; k++)
            file |= !strcmp(w, files[k]);
        for (int k = 0; values[k]; k++)
            value |= !strcmp(w, values[k]);
        if (w[0] != '-') {              /* this word is the source */
            if (have_src++)
                return ros_error(ERR_BOX, "RosAsm: one source (%s?)", w);
            if ((e = map(w, mapped[nm], sizeof mapped[nm])) || (e = add(&l, "RosAsm", mapped[nm++])))
                return e;
            continue;
        }
        if ((e = add(&l, "RosAsm", w)))
            return e;
        if (file || value) {
            if (i + 1 >= ws.n)
                return ros_error(ERR_BOX, "RosAsm: %s needs a value", w);
            const char *v = ws.w[++i];
            if (file) {
                if ((e = map(v, mapped[nm], sizeof mapped[nm])))
                    return e;
                v = mapped[nm++];
            }
            if ((e = add(&l, "RosAsm", v)))
                return e;
        }
    }
    if (!have_src)
        return ros_error(ERR_BOX, "Syntax: *RosAsm <source> [rosasm options...]");
    int st;
    if ((e = run("RosAsm", &l, &st)))
        return e;
    return st ? ros_error(ERR_BOX + 3, "RosAsm: the translation failed") : NULL;
}

/* ---- *RosBas ------------------------------------------------------------------------ */

/* x32-tcc's options for rosbas's C. They give RISC OS's C as the ROM
 * library's clients see it, laid out as the clang-built runtime is. The
 * kit's README describes this: rb_tcc.h is forced in and packs structures
 * as clang's -fpack-struct=4 does. */
static const char *const rosbas_cflags[] = {
    "-funsigned-char", "-mms-bitfields", "-std=c11", "-nostdinc", "-w",
    "-D__riscos", "-D__riscos__", "-D__APCS_32",
    "-U__linux__", "-U__linux", "-Ulinux", "-U__gnu_linux__", "-U__unix__", "-U__unix", "-Uunix", NULL,
};

/* One step of the build. The line is run and then its status is checked. */
static os_error *step(struct line *l, const char *what, const char *src)
{
    int st;
    os_error *e = run("RosBas", l, &st);
    if (e)
        return e;
    return st ? ros_error(ERR_BOX + 3, "RosBas: %s %s", src, what) : NULL;
}

static os_error *cmd_rosbas(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    static struct words ws;
    os_error *e = split("RosBas", tail, &ws);
    if (e)
        return e;
    static char src[1024], out[1024], dirs[MAXW][1024];
    static const char *quirks[MAXW];
    int ndirs = 0, nquirks = 0;
    const char *src_name = NULL, *out_name = NULL;
    for (int i = 0; i < ws.n; i++) {
        const char *w = ws.w[i];
        if (!strcmp(w, "-o") || !strcmp(w, "-L") || !strcmp(w, "--quirk") || !strcmp(w, "--quirks")) {
            if (i + 1 >= ws.n)
                return ros_error(ERR_BOX, "RosBas: %s needs a value", w);
            const char *v = ws.w[++i];
            if (!strcmp(w, "-o"))
                out_name = v;
            else if (!strcmp(w, "-L")) {
                if ((e = map(v, dirs[ndirs], sizeof dirs[ndirs])))
                    return e;
                ndirs++;
            } else
                quirks[nquirks++] = v;
        } else if (w[0] == '-') {
            return ros_error(ERR_BOX, "RosBas: %s is not an option (-o, -L, --quirk, --quirks)", w);
        } else if (src_name) {
            return ros_error(ERR_BOX, "RosBas: one source (%s?)", w);
        } else {
            src_name = w;
        }
    }
    if (!src_name)
        return ros_error(ERR_BOX, "Syntax: *RosBas <source> [-o <program>] [-L <directory>]... [--quirk <name>]...");
    char prog[1024];
    if (!out_name) {
        if (!program_name(src_name, "bas", prog, sizeof prog))
            return ros_error(ERR_BOX + 2, "RosBas: %s is not bas.<name> or <name>/bas: give -o", src_name);
        out_name = prog;
    }
    if ((e = map(src_name, src, sizeof src)) || (e = map(out_name, out, sizeof out)))
        return e;
    if (!strcmp(src, out))
        return ros_error(ERR_BOX + 2, "RosBas: the program would be written over %s", src_name);
    /* The C file and its object go beside the program and are removed at the end. */
    static char c_file[1100], o_file[1100], inc1[1100], inc2[1100], lib[1100], forced[1100];
    snprintf(c_file, sizeof c_file, "%s.rosbas.c", out);
    snprintf(o_file, sizeof o_file, "%s.rosbas.o", out);
    snprintf(forced, sizeof forced, "%s/include/rb_tcc.h", BOXTOOLS_ROSBAS_KIT);
    snprintf(inc1, sizeof inc1, "-I%s/include", BOXTOOLS_ROSBAS_KIT);
    snprintf(inc2, sizeof inc2, "-I%s", BOXTOOLS_CAPPS_INC);
    snprintf(lib, sizeof lib, "%s/librb.a", BOXTOOLS_ROSBAS_KIT);
    static struct line l;
    /* 1. Compile BASIC to C. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "RosBas", BOXTOOLS_ROSBAS)) || (e = add(&l, "RosBas", "compile")) ||
        (e = add(&l, "RosBas", src)) || (e = add(&l, "RosBas", "-o")) || (e = add(&l, "RosBas", c_file)))
        return e;
    for (int i = 0; i < ndirs; i++)
        if ((e = add(&l, "RosBas", "-L")) || (e = add(&l, "RosBas", dirs[i])))
            return e;
    for (int i = 0; i < nquirks; i++)
        if ((e = add(&l, "RosBas", "--quirk")) || (e = add(&l, "RosBas", quirks[i])))
            return e;
    if ((e = step(&l, "did not compile", src_name)))
        return e;
#if BOXTOOLS_CLANG_BOX
    /* 2. Compile the C to an object for the box's ABI. The compile uses
     *    clang's default target and config (the flag table the kit was
     *    built with), with the kit's headers before RISC OS's. */
    (void)forced, (void)rosbas_cflags;
    snprintf(inc1, sizeof inc1, "%s/include", BOXTOOLS_ROSBAS_KIT);
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "RosBas", BOXTOOLS_CLANG)) || (e = add(&l, "RosBas", "-w")) ||
        (e = add(&l, "RosBas", "-isystem")) || (e = add(&l, "RosBas", inc1)) ||
        (e = add(&l, "RosBas", "-isystem")) || (e = add(&l, "RosBas", BOXTOOLS_CAPPS_INC)) ||
        (e = add(&l, "RosBas", "-c")) || (e = add(&l, "RosBas", c_file)) || (e = add(&l, "RosBas", "-o")) ||
        (e = add(&l, "RosBas", o_file)))
        goto done;
    if ((e = step(&l, "did not compile as C (clang)", src_name)))
        goto done;
    /* 3. Link the object and the runtime into an application against the
     *    ROM library. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "RosBas", BOXTOOLS_ROSCC)) || (e = add(&l, "RosBas", "link")) ||
        (e = add(&l, "RosBas", "--clib")) || (e = add(&l, "RosBas", BOXTOOLS_CLANG_RT_FLAG)) ||
        (e = add(&l, "RosBas", BOXTOOLS_CLANG_RT)) || (e = add(&l, "RosBas", "-o")) ||
        (e = add(&l, "RosBas", out)) || (e = add(&l, "RosBas", o_file)) || (e = add(&l, "RosBas", lib)))
        goto done;
#else
    /* 2. Compile the C to an x32 object. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "RosBas", BOXTOOLS_X32TCC)))
        goto done;
    for (int i = 0; rosbas_cflags[i]; i++)
        if ((e = add(&l, "RosBas", rosbas_cflags[i])))
            goto done;
    /* The kit's headers come before RISC OS's. */
    if ((e = add(&l, "RosBas", "-include")) || (e = add(&l, "RosBas", forced)) ||
        (e = add(&l, "RosBas", inc1)) || (e = add(&l, "RosBas", inc2)) || (e = add(&l, "RosBas", "-c")) ||
        (e = add(&l, "RosBas", c_file)) || (e = add(&l, "RosBas", "-o")) || (e = add(&l, "RosBas", o_file)))
        goto done;
    if ((e = step(&l, "did not compile as C (x32-tcc)", src_name)))
        goto done;
    /* 3. Link the object, the runtime and tcc's helpers into an
     *    application against the ROM library. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "RosBas", BOXTOOLS_ROSCC)) || (e = add(&l, "RosBas", "link")) ||
        (e = add(&l, "RosBas", "--clib")) || (e = add(&l, "RosBas", "--x32-rt")) ||
        (e = add(&l, "RosBas", BOXTOOLS_X32_RT)) || (e = add(&l, "RosBas", "-o")) || (e = add(&l, "RosBas", out)) ||
        (e = add(&l, "RosBas", o_file)) || (e = add(&l, "RosBas", lib)) ||
        (e = add(&l, "RosBas", BOXTOOLS_X32_LIBTCC)))
        goto done;
#endif
    if ((e = step(&l, "did not link (roscc)", src_name)))
        goto done;
    e = set_type(out_name, TYPE_APP);          /* an application: *Run, or its name */
done:
    unlink(c_file);
    unlink(o_file);
    return e;
}

/* ---- *RosBasRun ------------------------------------------------------------------- */

/* The Apple Silicon box has rosbas's JIT. rosbas run --target a64x32
 * compiles a program straight to an A64X32 application image, with no C,
 * no clang and no link program (rosbas design/jit.md). The Intel box has
 * no JIT for x32. There *RosBasRun is *RosBas to the scrap image, and then
 * a run. Hosted, the image could not run, because the hosted runtime runs
 * no 32-bit code. So the command is refused. */
#if BOXTOOLS_CLANG_BOX && defined(__aarch64__)
#define ROSBASRUN_JIT 1
#elif BOXTOOLS_CLANG_BOX
#define ROSBASRUN_JIT 0
#else
#define ROSBASRUN_HOSTED 1
#endif

#define ROSBASRUN_SYNTAX "RosBasRun <source> [-o <image>] [-L <directory>]... [--quirk <name>]... [--no-run]"

/* Runs a command line with OS_CLI. Returns its error, or NULL. */
static os_error *oscli(const char *line)
{
    size_t n = strlen(line);
    char *b = ros_rma_alloc((uint32_t)n + 1);
    if (!b)
        return ros_error(ROS_ERR_NO_ROOM_IN_RMA, "No room in RMA");
    memcpy(b, line, n);
    b[n] = '\r';
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b);
    ros_swi(&c, XOS_CLI);
    ros_rma_free(b);
    return c.v ? ros_ptr(c.r[0]) : NULL;
}

static os_error *cmd_rosbasrun(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    static struct words ws;
    os_error *e = split("RosBasRun", tail, &ws);
    if (e)
        return e;
    static const char *dirs[MAXW], *quirks[MAXW];
    int ndirs = 0, nquirks = 0, norun = 0;
    const char *src_name = NULL, *out_name = NULL;
    for (int i = 0; i < ws.n; i++) {
        const char *w = ws.w[i];
        if (!strcmp(w, "-o") || !strcmp(w, "-L") || !strcmp(w, "--quirk") || !strcmp(w, "--quirks")) {
            if (i + 1 >= ws.n)
                return ros_error(ERR_BOX, "RosBasRun: %s needs a value", w);
            const char *v = ws.w[++i];
            if (!strcmp(w, "-o"))
                out_name = v;
            else if (!strcmp(w, "-L"))
                dirs[ndirs++] = v;
            else
                quirks[nquirks++] = v;
        } else if (!strcmp(w, "--no-run")) {
            norun = 1;
        } else if (w[0] == '-') {
            return ros_error(ERR_BOX, "RosBasRun: %s is not an option (-o, -L, --quirk, --quirks, --no-run)", w);
        } else if (src_name) {
            return ros_error(ERR_BOX, "RosBasRun: one source (%s?)", w);
        } else {
            src_name = w;
        }
    }
    if (!src_name)
        return ros_error(ERR_BOX, "Syntax: *" ROSBASRUN_SYNTAX);
    if (norun && !out_name)
        return ros_error(ERR_BOX, "RosBasRun: --no-run keeps the image only with -o <image>");
#ifdef ROSBASRUN_HOSTED
    (void)dirs, (void)quirks, (void)ndirs, (void)nquirks, (void)oscli, (void)remove_file;
    return ros_error(ERR_BOX + 5, "RosBasRun: the hosted runtime runs no 32-bit code: use *RosBas, and run the "
                                  "application in the box");
#else
    /* The image is -o's file, which is kept. Otherwise it is a scrap image,
     * named Wimp$ScrapDir's RosBasRun if that is set, else the source's name
     * and _jit. A scrap image is removed after the run. In a task window
     * the run does not come back here, because the program's exit ends the
     * task. Then the scrap image is left until the next *RosBasRun writes
     * over it. */
    static char img_name[1100], img[1024], src[1024], scrap[1024];
    if (out_name)
        snprintf(img_name, sizeof img_name, "%s", out_name);
    else if (varval("Wimp$ScrapDir", scrap, sizeof scrap))
        snprintf(img_name, sizeof img_name, "%s.RosBasRun", scrap);
    else
        snprintf(img_name, sizeof img_name, "%s_jit", src_name);
    if ((e = map(src_name, src, sizeof src)) || (e = map(img_name, img, sizeof img)))
        return e;
    if (!strcmp(src, img))
        return ros_error(ERR_BOX + 2, "RosBasRun: the image would be written over %s", src_name);
    static struct line l;
    l.n = 0, l.s[0] = 0;
#if ROSBASRUN_JIT
    /* rosbas compiles the program to an application image in memory and
     * writes it. */
    static char mapped[MAXW][1024];
    if ((e = add(&l, "RosBasRun", BOXTOOLS_ROSBAS)) || (e = add(&l, "RosBasRun", "run")) ||
        (e = add(&l, "RosBasRun", "--target")) || (e = add(&l, "RosBasRun", "a64x32")) ||
        (e = add(&l, "RosBasRun", "--a64x32-rt")) || (e = add(&l, "RosBasRun", BOXTOOLS_A64X32_RT)) ||
        (e = add(&l, "RosBasRun", src)) || (e = add(&l, "RosBasRun", "-o")) || (e = add(&l, "RosBasRun", img)))
        return e;
    for (int i = 0; i < ndirs; i++)
        if ((e = map(dirs[i], mapped[i], sizeof mapped[i])) || (e = add(&l, "RosBasRun", "-L")) ||
            (e = add(&l, "RosBasRun", mapped[i])))
            return e;
    for (int i = 0; i < nquirks; i++)
        if ((e = add(&l, "RosBasRun", "--quirk")) || (e = add(&l, "RosBasRun", quirks[i])))
            return e;
    int st;
    if ((e = run("RosBasRun", &l, &st)))
        goto done;
    if (st) {
        e = ros_error(ERR_BOX + 3, "RosBasRun: %s did not compile", src_name);
        goto done;
    }
    if ((e = set_type(img_name, TYPE_APP)))
        goto done;
#else
    /* On the Intel box, *RosBas makes the image. */
    if ((e = add(&l, "RosBasRun", "RosBas")) || (e = add(&l, "RosBasRun", src_name)) ||
        (e = add(&l, "RosBasRun", "-o")) || (e = add(&l, "RosBasRun", img_name)))
        return e;
    for (int i = 0; i < ndirs; i++)
        if ((e = add(&l, "RosBasRun", "-L")) || (e = add(&l, "RosBasRun", dirs[i])))
            return e;
    for (int i = 0; i < nquirks; i++)
        if ((e = add(&l, "RosBasRun", "--quirk")) || (e = add(&l, "RosBasRun", quirks[i])))
            return e;
    if ((e = oscli(l.s)))
        goto done;
#endif
    if (!norun) {
        /* Run the image as *Run runs an application, here and in its slot.
         * If it ends with an error, that error is this command's. */
        l.n = 0, l.s[0] = 0;
        if (!(e = add(&l, "RosBasRun", "Run")) && !(e = add(&l, "RosBasRun", img_name)))
            e = oscli(l.s);
    }
done:
    /* Use its RISC OS name. It is typed &FF8, so its host file's name has changed. */
    if (!out_name)
        remove_file(img_name);
    return e;
#endif
}

/* ---- *Mojo ------------------------------------------------------------------------ */

/* The Mojo target and runtime objects, for each box. x32 and A64X32 share
 * the pipeline. mojo compiles, and then roscc links with the staged
 * freestanding runtime and the gate shims. The runtime is rostrt, which has
 * the KGEN_CompilerRT surface, the OS_Heap heap and write(). On A64X32, x18
 * is the static base (roscc's link checks this), so the compiler reserves
 * it.
 *
 * x32 compiles straight to an object. A64X32 goes through LLVM IR (#173).
 * roscc mojo-ir rewrites the IR as LLVM's back end needs it for an ILP32
 * program. There are two changes. Addressing wraps at 4 GB, as
 * -fwrapv-pointer makes it for C, so that a List[String] passes roscc's
 * addressing lint. A large result is returned through an explicit sret
 * argument, as clang's front end returns one, because LLVM's own demotion
 * reads each pointer back as 8 bytes (roscc src/mojoir.rs). The box's
 * clang then compiles the IR to the object, with A64X32's code flags. The
 * machine decides this, and not which C compiler *CC has. */
#if defined(__aarch64__) && !defined(ROS_ARENA_HOSTED)
#define MOJO_A64X32   1
#define MOJO_TRIPLE   "aarch64-unknown-linux-gnu_ilp32"
#define MOJO_FEATURES "+reserve-x18"    /* x18: the static base */
#define MOJO_RT       "/usr/lib/mojo/rt_a64x32"
#define MOJO_VIA_IR   1
#else
#define MOJO_A64X32   0
#define MOJO_TRIPLE   "x86_64-unknown-linux-gnux32"
#define MOJO_FEATURES ""
#define MOJO_RT       "/usr/lib/mojo/rt_x32"
#define MOJO_VIA_IR   0
#endif

/* When compiling for any machine, the standard library asks whether the
 * target has x86's features (CompilationTarget.has_sse4 and has_avx512f,
 * which select the x86 paths of FPUtils, SIMD and the maths). For AArch64,
 * Mojo warns that it does not know the feature 'sse4.1', on every program.
 * The answer, false, is right, because nothing passes the feature
 * (MOJO_FEATURES is +reserve-x18 alone). The warning is not about the
 * program, so it is not shown. */
#define MOJO_NOISE "is not a recognized target feature name; this check always evaluates to false"

/* One step of the build: the line run, then its status checked */
static os_error *mojo_step(struct line *l, const char *what, const char *src)
{
    int st;
    os_error *e = run_dropping("Mojo", l, MOJO_NOISE, &st);
    if (e)
        return e;
    return st ? ros_error(ERR_BOX + 3, "Mojo: %s %s", src, what) : NULL;
}


static os_error *cmd_mojo(struct ros_module *m, uint32_t tail, uint32_t argc)
{
    (void)m, (void)argc;
    static struct words ws;
    os_error *e = split("Mojo", tail, &ws);
    if (e)
        return e;
    static char src[1024], out[1024];
    const char *src_name = NULL, *out_name = NULL;
    int kernel = 0;
    for (int i = 0; i < ws.n; i++) {
        const char *w = ws.w[i];
        if (!strcmp(w, "-o")) {
            if (++i >= ws.n)
                return ros_error(ERR_BOX, "Mojo: -o needs a value");
            out_name = ws.w[i];
        } else if (!strcmp(w, "--kernel")) {
            kernel = 1;
        } else if (w[0] == '-') {
            return ros_error(ERR_BOX, "Mojo: %s is not an option (-o, --kernel)", w);
        } else if (src_name) {
            return ros_error(ERR_BOX, "Mojo: one source (%s?)", w);
        } else {
            src_name = w;
        }
    }
    if (!src_name)
        return ros_error(ERR_BOX, "Syntax: *Mojo [--kernel] <source> [-o <program>]");
    char prog[1024];
    if (!out_name) {
        if (!program_name(src_name, "mojo", prog, sizeof prog))
            return ros_error(ERR_BOX + 2, "Mojo: %s is not mojo.<name> or <name>/mojo: give -o", src_name);
        out_name = prog;
    }
    if ((e = map(src_name, src, sizeof src)) || (e = map(out_name, out, sizeof out)))
        return e;
    /* The compiler reads .mojo files. A mojo.<name> source maps to
     * <dir>/<name> on the Linux side, so the suffix is added back for it. */
    static char mojosrc[1100];
    size_t srclen = strlen(src);
    if (srclen + 6 < sizeof mojosrc && strcmp(src + (srclen > 5 ? srclen - 5 : 0), ".mojo")) {
        snprintf(mojosrc, sizeof mojosrc, "%s.mojo", src);
        if (access(mojosrc, R_OK) == 0)
            snprintf(src, sizeof src, "%s", mojosrc);
    }
    if (!strcmp(src, out))
        return ros_error(ERR_BOX + 2, "Mojo: the program would be written over %s", src_name);
    char mojo[1024];
    if (!varval("Mojo$Exe", mojo, sizeof mojo))
        snprintf(mojo, sizeof mojo, "%s", BOXTOOLS_MOJO);
    if (access(mojo, X_OK) != 0)
        return ros_error(ERR_BOX + 5, "Mojo: there is no Mojo compiler here (%s)", mojo);
    static char obj[1100], ir[1100], tmpdir[1100], tmpsrc[1200], srcdir[1024];
    snprintf(obj, sizeof obj, "%s.mojo.o", out);
    snprintf(ir, sizeof ir, "%s.mojo.ll", out);
    tmpsrc[0] = 0;
    /* The source's own directory, where the modules that it imports are */
    snprintf(srcdir, sizeof srcdir, "%s", src);
    char *slash = strrchr(srcdir, '/');
    if (slash && slash != srcdir)
        *slash = 0;
    /* mojo reads only a file whose name ends .mojo. A source named the
     * RISC OS way is compiled from a copy. Such a source is mojo.hello, a
     * host file with no extension, or one typed &A6E by its ,a6e suffix.
     * The copy is <program>.mojo-src's <leaf>.mojo, and it is removed
     * afterwards. */
    const char *compile_src = src;
    size_t sl = strlen(src);
    if (sl < 5 || strcmp(src + sl - 5, ".mojo")) {
        const char *leaf = slash ? strrchr(src, '/') + 1 : src;
        size_t ll = strlen(leaf);
        if (ll > 4 && leaf[ll - 4] == ',')
            ll -= 4;
        snprintf(tmpdir, sizeof tmpdir, "%s.mojo-src", out);
        snprintf(tmpsrc, sizeof tmpsrc, "%s/%.*s.mojo", tmpdir, (int)ll, leaf);
        FILE *in = fopen(src, "rb");
        if (!in)
            return ros_error(ERR_BOX + 4, "Mojo: %s cannot be read", src_name);
        mkdir(tmpdir, 0755);
        FILE *cp = fopen(tmpsrc, "wb");
        char buf[4096];
        size_t k;
        while (cp && (k = fread(buf, 1, sizeof buf, in)) > 0)
            if (fwrite(buf, 1, k, cp) != k)
                break;
        fclose(in);
        if (!cp || fclose(cp)) {
            unlink(tmpsrc);
            rmdir(tmpdir);
            return ros_error(ERR_BOX + 1, "Mojo: %s cannot be copied for the compiler", src_name);
        }
        compile_src = tmpsrc;
    }
    /* The PTY's children get a fixed environment (pty.c builds its own).
     * So the compiler is told on the command line where its packages are.
     * These are the precompiled stdlib, then the riscos and demos packages.
     * --emit object never uses the linker probe, and the image's
     * /etc/modular/modular.cfg linker_driver setting skips it. */
    static struct line l;
    /* 1. Compile Mojo to an object for the box. On A64X32 it goes to IR, below. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "Mojo", mojo)) || (e = add(&l, "Mojo", "build")) ||
        (e = add(&l, "Mojo", "--emit")) || (e = add(&l, "Mojo", MOJO_VIA_IR ? "llvm" : "object")) ||
        (e = add(&l, "Mojo", "--target-triple")) || (e = add(&l, "Mojo", MOJO_TRIPLE)))
        goto done;
#if MOJO_A64X32
    if ((e = add(&l, "Mojo", "--target-features")) || (e = add(&l, "Mojo", MOJO_FEATURES)))
        goto done;
#endif
    /* The riscos package's bindings were written before Mojo 1.1
     * deprecated UnsafePointer and its positional indexing, pointer
     * arithmetic, bitcast and unsafe_ptr. Those warnings are about the
     * package, not the program, and each import of the package gave dozens. */
    static const char *const deprecated[] = {
        "UnsafePointer", "Pointer.__getitem__", "Pointer.__add__", "Pointer.bitcast",
        "StringLiteral.unsafe_ptr", NULL,
    };
    for (int i = 0; deprecated[i]; i++)
        if ((e = add(&l, "Mojo", "--ignore-deprecated")) || (e = add(&l, "Mojo", deprecated[i])))
            goto done;
    if ((e = add(&l, "Mojo", "-I")) || (e = add(&l, "Mojo", BOXTOOLS_MOJO_STD)) ||
        (e = add(&l, "Mojo", "-I")) || (e = add(&l, "Mojo", BOXTOOLS_MOJO_PKG)) ||
        (e = add(&l, "Mojo", "-I")) || (e = add(&l, "Mojo", srcdir)) ||
        (e = add(&l, "Mojo", compile_src)) || (e = add(&l, "Mojo", "-o")) ||
        (e = add(&l, "Mojo", MOJO_VIA_IR ? ir : obj)))
        goto done;
    if ((e = mojo_step(&l, "did not compile", src_name)))
        goto done;
#if MOJO_VIA_IR
    /* 1b. Rewrite the IR for A64X32 (roscc mojo-ir), in place. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "Mojo", BOXTOOLS_ROSCC)) || (e = add(&l, "Mojo", "mojo-ir")) ||
        (e = add(&l, "Mojo", ir)) || (e = add(&l, "Mojo", "-o")) || (e = add(&l, "Mojo", ir)))
        goto done;
    if ((e = mojo_step(&l, "did not compile (roscc mojo-ir)", src_name)))
        goto done;
    /* 1c. Compile the IR with the box's clang. It uses A64X32's code flags
     * (the target and -O2 from abi/a64x32/flags.mk). It does not use the C
     * config file, because that file's C-only flags would each give a
     * warning for IR. */
    l.n = 0, l.s[0] = 0;
    static const char *const ir_cc[] = {
        "--no-default-config", "--target=" MOJO_TRIPLE, "-O2", "-fno-pic", "-ffixed-x18",
        "-mno-outline-atomics", "-Wno-override-module", "-c", NULL,
    };
    if ((e = add(&l, "Mojo", BOXTOOLS_CLANG)))
        goto done;
    for (int i = 0; ir_cc[i]; i++)
        if ((e = add(&l, "Mojo", ir_cc[i])))
            goto done;
    if ((e = add(&l, "Mojo", ir)) || (e = add(&l, "Mojo", "-o")) || (e = add(&l, "Mojo", obj)))
        goto done;
    if ((e = mojo_step(&l, "did not compile (clang, from Mojo's IR)", src_name)))
        goto done;
#endif
    /* 2k. With --kernel, make a Worker kernel file (roscc link --kernel).
     *     It is the object alone, typed Data. The source's @export def
     *     kernel is the entry. It may call nothing (no runtime, no SWIs)
     *     and has no statics. roscc's link and the Worker module's checker
     *     require this of any kernel (modules/worker/README.md, "Kernels"). */
    if (kernel) {
        l.n = 0, l.s[0] = 0;
        if ((e = add(&l, "Mojo", BOXTOOLS_ROSCC)) || (e = add(&l, "Mojo", "link")) ||
            (e = add(&l, "Mojo", "--kernel")) || (e = add(&l, "Mojo", "--entry")) ||
            (e = add(&l, "Mojo", "kernel")) || (e = add(&l, "Mojo", "-o")) || (e = add(&l, "Mojo", out)) ||
            (e = add(&l, "Mojo", obj)))
            goto done;
        if ((e = mojo_step(&l, "did not link as a Worker kernel (roscc link --kernel)", src_name)))
            goto done;
        e = set_type(out_name, TYPE_DATA);
        goto done;
    }
    /* 2. Link the object and the staged runtime into an application. It is
     *    freestanding and uses nothing from the ROM. rostrt carries the OS
     *    calls and Mojo's CompilerRT surface, and the shims carry the gate. */
    l.n = 0, l.s[0] = 0;
    if ((e = add(&l, "Mojo", BOXTOOLS_ROSCC)) || (e = add(&l, "Mojo", "link")) ||
        (e = add(&l, "Mojo", "--entry")) || (e = add(&l, "Mojo", "_start")) ||
        (e = add(&l, "Mojo", "-o")) || (e = add(&l, "Mojo", out)))
        goto done;
#if MOJO_A64X32
    if ((e = add(&l, "Mojo", MOJO_RT "/crt0_mojo_a64x32.o")) ||
        (e = add(&l, "Mojo", obj)) ||
        (e = add(&l, "Mojo", MOJO_RT "/swis_os_a64x32.o")) ||
        (e = add(&l, "Mojo", MOJO_RT "/swis_wimp_a64x32.o")) ||
        (e = add(&l, "Mojo", MOJO_RT "/wimp_a64x32.o")) ||
        (e = add(&l, "Mojo", MOJO_RT "/maths_a64x32.a")) ||
        (e = add(&l, "Mojo", MOJO_RT "/swis_a64x32.a")) ||
        (e = add(&l, "Mojo", MOJO_RT "/rostrt_a64x32.o")))
#else
    if ((e = add(&l, "Mojo", MOJO_RT "/crt0_mojo_x32.o")) ||
        (e = add(&l, "Mojo", obj)) ||
        (e = add(&l, "Mojo", MOJO_RT "/swis_os_x32.o")) ||
        (e = add(&l, "Mojo", MOJO_RT "/swis_wimp_x32.o")) ||
        (e = add(&l, "Mojo", MOJO_RT "/wimp_x32.o")) ||
        (e = add(&l, "Mojo", MOJO_RT "/maths_x32.a")) ||
        (e = add(&l, "Mojo", MOJO_RT "/swis_x32.a")) ||
        (e = add(&l, "Mojo", MOJO_RT "/rostrt_x32.o")))
#endif
        goto done;
    if ((e = mojo_step(&l, "did not link (roscc)", src_name)))
        goto done;
    e = set_type(out_name, TYPE_APP);
done:
    unlink(obj);
#if MOJO_VIA_IR
    unlink(ir);
#endif
    if (tmpsrc[0]) {
        unlink(tmpsrc);
        rmdir(tmpdir);
    }
    return e;
}

/* ---- the module ------------------------------------------------------------------------ */

#define C(n, lo, syntax, help, fn) { n, ROS_CMD_INFO(lo, 255, 0, 0), "Syntax: *" syntax, help, fn }

static const struct ros_command commands[] = {
    C("CC", 1, CC_SYNTAX, CC_HELP, cmd_cc),
    C("RosAsm", 1, "RosAsm <source> [rosasm options...]",
      "*RosAsm translates RISC OS assembler with A7232ToolChain's rosasm, on RISC OS names.",
      cmd_rosasm),
    C("RosBas", 1, "RosBas <source> [-o <program>] [-L <directory>]... [--quirk <name>]...", ROSBAS_HELP,
      cmd_rosbas),
    C("RosBasRun", 1, ROSBASRUN_SYNTAX, ROSBASRUN_HELP, cmd_rosbasrun),
    C("Mojo", 1, "Mojo [--kernel] <source> [-o <program>]",
      "*Mojo compiles a Mojo program into a RISC OS application (mojo, roscc, " MOJO_WHAT
      "): mojo.hello's is hello, as is hello/mojo's.  --kernel makes a Worker kernel "
      "of its @export def kernel.",
      cmd_mojo),
    C("RunBox", 1, "RunBox <program> [arguments...]",
      "*RunBox runs a program *CC made, on the text window; its exit status is Sys$ReturnCode.",
      cmd_runbox),
    { 0 },
};

/* Sets the run action of an ELF file to *RunBox, as a BASIC file runs by
 * *BASIC. Also names the type &A6E, so that Mojo source shows as Mojo. */
static os_error *init(struct ros_module *m, const char *tail)
{
    (void)m, (void)tail;
    default_var("Alias$@RunType_E1F", "RunBox %*0");
    default_var("File$Type_A6E", "Mojo");
    return NULL;
}

struct ros_module boxtools_module = {
    .title = "BoxTools",
    .init = init,
    .help = "BoxTools\t1.04 (9 Oct 2026) ROSGD native: *CC (" CC_WHAT "), *RunBox, *RosAsm, *RosBas, *RosBasRun, *Mojo",
    .commands = commands,
};
