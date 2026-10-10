/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_boxtools.c: BoxTools (modules/boxtools).
 *
 * *CC compiles a program with clang (in a box) or tcc (hosted). *RunBox
 * runs it with its arguments as typed and makes its exit status
 * Sys$ReturnCode.
 *
 * The test also checks the guards. These are a program that would be
 * written over its source, a source *CC cannot name a program for, a
 * compile that fails and a program that is not there. It checks *RosAsm
 * too. Hosted, where rosasm is, it assembles. In the box, where rosasm is
 * not, it refuses plainly.
 *
 * Hosted, *CC is the Mac's tcc (deps/build-tcc.sh, host/), making Linux
 * programs. In either box *CC is clang. The Intel box's *CC replaced tcc.
 * It makes an A64X32 or x32 application by default and a Linux program
 * with --linux. There *RosBas is checked too. In either box, *Mojo
 * compiles a program to an application, which runs by its name, and
 * refuses one that does not compile.
 *
 * The files live in a directory of the test's own (TMPDIR, or the box's
 * /tmp), as the disc BTTest, and go when it ends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boxtools.h"
#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/dynarea.h"
#include "rosgd/environment.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "rosgd/platform.h"
#include "selftest.h"

#define check ros_check

static int cli(const char *line)
{
    size_t n = strlen(line);
    char *c = ros_rma_alloc((uint32_t)n + 1);
    memcpy(c, line, n);
    c[n] = '\r';
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(c);
    ros_swi(&s, XOS_CLI);
    ros_rma_free(c);
    return s.v ? (int)((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int get(const char *path, char *out, size_t max)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(out, 1, max - 1, f);
    out[n] = 0;
    fclose(f);
    return 1;
}

/* Sys$ReturnCode, as a number */
static int return_code(void)
{
    char *b = ros_rma_alloc(64);
    memcpy(b + 32, "Sys$ReturnCode", 15);
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ros_addr(b + 32), c.r[1] = ros_addr(b), c.r[2] = 31, c.r[3] = 0, c.r[4] = 3;
    ros_swi(&c, XOS_ReadVarVal);
    int rc = c.v ? -1 : atoi(b);
    ros_rma_free(b);
    return rc;
}

static void names(void)
{
    char o[64];
    int a = boxtools_program_name("c.hello", o, sizeof o) && !strcmp(o, "hello");
    int b = boxtools_program_name("$.c.proj.c.x", o, sizeof o) && !strcmp(o, "$.c.proj.x");
    int c = boxtools_program_name("C.hello", o, sizeof o) && !strcmp(o, "hello");
    int d = boxtools_program_name("hello/c", o, sizeof o) && !strcmp(o, "hello");
    int e = !boxtools_program_name("$.src.main", o, sizeof o);
    check(a && b && c && d && e,
          "BoxTools: a source's program -- c.hello hello, the last c. left out, hello/c hello; "
          "$.src.main none", "%d%d%d%d%d", a, b, c, d, e);
}

void ros_selftest_boxtools(void)
{
    names();

    char dir[256], p[400], out[400], got[256];
    const char *t = getpid() == 1 ? "/tmp" : getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rosgd-boxtools-%d", t ? t : "/tmp", (int)getpid());
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/c", dir);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/src", dir);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/c/hello", dir);
    put(p, "#include <stdio.h>\n"
           "int main(int argc, char **argv)\n"
           "{\n"
           "    FILE *f = fopen(argv[2], \"w\");\n"
           "    if (!f) return 1;\n"
           "    fprintf(f, \"%d %s\", argc, argv[1]);\n"
           "    fclose(f);\n"
           "    printf(\"hello from a program *CC made\\n\");\n"
           "    return 7;\n"
           "}\n");
    snprintf(p, sizeof p, "%s/c/bad", dir);
    put(p, "int main(void) { return undeclared; }\n");
    snprintf(p, sizeof p, "%s/src/main", dir);
    put(p, "int main(void) { return 0; }\n");
    ros_hostfs_mount("BTTest", dir);

#if !defined(ROS_ARENA_HOSTED)
    /* Either box's *CC is clang (the Intel box's since it replaced tcc).
     * By default it makes a RISC OS application, A64X32 or x32, linked by
     * roscc against the ROM's SharedCLibrary, typed &FF8 and run by its
     * name. With --linux it makes a Linux program, run by *RunBox. The
     * same source makes both. */
#if defined(__aarch64__)
#define BT_ABI "A64X32"
#define BT_LINUX "aarch64-linux-musl"
#else
#define BT_ABI "x32"
#define BT_LINUX "x86_64-linux-musl"
#endif
    char line[600];
    /* A rosbas program needs a slot of 1024K at least (the kit's README).
     * The first application, hello, runs in 512K. *WimpSlot -next 2048K
     * then gives the *RosBas one its room, as it would on RISC OS. The
     * console task's space is made the next size and is not kept at the
     * first's (#45). After that the default, 1024K, applies again. */
    int had_slot = ros_slot_current != NULL;
    struct ros_environment env;
    ros_env_save(&env);
    cli("WimpSlot -next 512K");
    int cc = cli("CC HostFS::BTTest.$.c.hello");
    snprintf(p, sizeof p, "%s/hello,ff8", dir);                 /* typed &FF8, as HostFS keeps types */
    int app = access(p, F_OK) == 0;
    snprintf(p, sizeof p, "%s/hello.cc0.o", dir);
    int tidy = access(p, F_OK) != 0;
    check(!cc && app && tidy,
          "*CC -- c.hello compiled " BT_ABI " by clang and linked by roscc against the ROM's SharedCLibrary: "
          "the application hello beside c, typed &FF8, its object gone",
          "&%X, app %d, tidy %d", cc, app, tidy);

    snprintf(out, sizeof out, "%s/out", dir);
    int byname = cli("HostFS::BTTest.$.hello 42 HostFS::BTTest.$.out");  /* by name: an application */
    int rc = return_code();
    int there = get(out, got, sizeof got);
    check(!byname && rc == 7 && there && !strcmp(got, "3 42"),
          "*CC's application runs by its name: its arguments through the C library, its file written, "
          "exit(7) Sys$ReturnCode",
          "&%X, Sys$ReturnCode %d, \"%s\"", byname, rc, there ? got : "(no file)");

    int lcc = cli("CC --linux HostFS::BTTest.$.c.hello -o HostFS::BTTest.$.lhello");
    snprintf(p, sizeof p, "%s/lhello,e1f", dir);
    check(!lcc && access(p, X_OK) == 0,
          "*CC --linux -- c.hello compiled and linked by clang for " BT_LINUX " (static, ld.lld): "
          "the Linux program lhello, typed ELF (&E1F)", "&%X", lcc);

    snprintf(out, sizeof out, "%s/out2", dir);
    snprintf(line, sizeof line, "RunBox HostFS::BTTest.$.lhello 42 %s", out);
    int rb = cli(line);
    rc = return_code();
    there = get(out, got, sizeof got);
    check(!rb && rc == 7 && there && !strcmp(got, "3 42"),
          "*RunBox -- the Linux program runs with its arguments as typed; its exit status is Sys$ReturnCode",
          "&%X, Sys$ReturnCode %d, \"%s\"", rb, rc, there ? got : "(no file)");

    snprintf(out, sizeof out, "%s/out3", dir);
    snprintf(line, sizeof line, "HostFS::BTTest.$.lhello 5 %s", out);  /* by name: its run action */
    int lbyname = cli(line);
    rc = return_code();
    there = get(out, got, sizeof got);
    check(!lbyname && rc == 7 && there && !strcmp(got, "3 5"),
          "BoxTools: an ELF program runs by its name -- Alias$@RunType_E1F is *RunBox", "&%X, %d, \"%s\"",
          lbyname, rc, there ? got : "(no file)");

    int bad = cli("CC HostFS::BTTest.$.c.bad");
    int lbad = cli("CC --linux HostFS::BTTest.$.c.bad");
    int noname = cli("CC HostFS::BTTest.$.src.main");
    int over = cli("CC HostFS::BTTest.$.src.main -o HostFS::BTTest.$.src.main");
    int lib = cli("CC HostFS::BTTest.$.c.hello -lm");
    int missing = cli("RunBox HostFS::BTTest.$.nothere");
    snprintf(p, sizeof p, "%s/src/main", dir);
    get(p, got, sizeof got);
    check(bad == 0xC0123 && lbad == 0xC0123 && noname == 0xC0122 && over == 0xC0122 && lib == 0xC0120 &&
              missing == 0xC0124 && !strncmp(got, "int main", 8),
          "*CC, *RunBox -- a failed compile (either kind), a source with no program name, a program over "
          "its source, -l for an application, a program not there: each refused, the source untouched",
          "&%X &%X &%X &%X &%X &%X", bad, lbad, noname, over, lib, missing);

    /* *RosBas: BBC BASIC to C (rosbas), A64X32 or x32 (clang), an application
     * (roscc with the kit's librb.a) */
    snprintf(p, sizeof p, "%s/bas", dir);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/bas/greet", dir);
    put(p, "10 PRINT \"hello from *RosBas\"\n"
           "20 F%=OPENOUT \"HostFS::BTTest.$.bout\"\n"
           "30 BPUT#F%,\"rosbas \"+STR$(6*7)\n"
           "40 CLOSE#F%\n"
           "50 QUIT 5\n");
    int rbas = cli("RosBas HostFS::BTTest.$.bas.greet");
    snprintf(p, sizeof p, "%s/greet,ff8", dir);
    int bapp = access(p, F_OK) == 0;
    cli("WimpSlot -next 2048K");
    int brun = cli("HostFS::BTTest.$.greet");
    rc = return_code();
    snprintf(out, sizeof out, "%s/bout,ffd", dir);            /* OPENOUT's file is typed data */
    there = get(out, got, sizeof got);
    check(!rbas && bapp && !brun && rc == 5 && there && !strcmp(got, "rosbas 42\n"),
          "*RosBas -- bas.greet made an " BT_ABI " application (rosbas, clang, roscc); run by its name, it "
          "wrote its file and QUIT 5 is Sys$ReturnCode",
          "&%X, app %d, run &%X, Sys$ReturnCode %d, \"%s\"", rbas, bapp, brun, rc, there ? got : "(no file)");
#if defined(__aarch64__)
    /* *RosBasRun: the same program compiled by rosbas's JIT to an image
     * in memory and run at once; -o keeps the image */
    remove(out);
    int jrun = cli("RosBasRun HostFS::BTTest.$.bas.greet -o HostFS::BTTest.$.jgreet");
    rc = return_code();
    there = get(out, got, sizeof got);
    snprintf(p, sizeof p, "%s/jgreet,ff8", dir);
    int japp = access(p, F_OK) == 0;
    snprintf(p, sizeof p, "%s/bas/rejected", dir);
    put(p, "10 N%=200:GOSUB N%\n20 END\n200 RETURN\n");   /* a computed line number: refused */
    int jbad = cli("RosBasRun HostFS::BTTest.$.bas.rejected");
    check(!jrun && japp && rc == 5 && there && !strcmp(got, "rosbas 42\n") && jbad == 0xC0123,
          "*RosBasRun -- bas.greet compiled by rosbas's JIT (no C) and run: its file written, QUIT 5 is "
          "Sys$ReturnCode, the image kept by -o; a program that does not compile is &C0123",
          "&%X, image %d, Sys$ReturnCode %d, \"%s\", bad &%X", jrun, japp, rc, there ? got : "(no file)", jbad);
#endif
    cli("WimpSlot -next 1024K");
    /* task 0 had no application space before these applications: given
     * back, as the tasks' checks expect */
    if (!had_slot && ros_slot_current) {
        ros_task_resize_own(0);
        ros_env_load(&env);
    }
#else
    int cc = cli("CC HostFS::BTTest.$.c.hello");
    snprintf(p, sizeof p, "%s/hello,e1f", dir);                 /* typed ELF, as HostFS keeps types */
    check(!cc && access(p, X_OK) == 0,
          "*CC -- c.hello compiled and linked by the Mac's tcc, the program hello beside c, typed ELF (&E1F)",
          "&%X", cc);

    snprintf(out, sizeof out, "%s/out", dir);
    char line[600];
    snprintf(line, sizeof line, "RunBox HostFS::BTTest.$.hello 42 %s", out);
    int rb = cli(line);
    int rc = return_code();
    int there = get(out, got, sizeof got);
    check(!rb && rc == 7 && there && !strcmp(got, "3 42"),
          "*RunBox -- the program runs with its arguments as typed; its exit status is Sys$ReturnCode",
          "&%X, Sys$ReturnCode %d, \"%s\"", rb, rc, there ? got : "(no file)");

    snprintf(out, sizeof out, "%s/out2", dir);
    snprintf(line, sizeof line, "HostFS::BTTest.$.hello 5 %s", out);  /* by name: its run action */
    int byname = cli(line);
    rc = return_code();
    there = get(out, got, sizeof got);
    check(!byname && rc == 7 && there && !strcmp(got, "3 5"),
          "BoxTools: an ELF program runs by its name -- Alias$@RunType_E1F is *RunBox", "&%X, %d, \"%s\"",
          byname, rc, there ? got : "(no file)");

    int bad = cli("CC HostFS::BTTest.$.c.bad");
    int noname = cli("CC HostFS::BTTest.$.src.main");
    int over = cli("CC HostFS::BTTest.$.src.main -o HostFS::BTTest.$.src.main");
    int missing = cli("RunBox HostFS::BTTest.$.nothere");
    snprintf(p, sizeof p, "%s/src/main", dir);
    get(p, got, sizeof got);
    check(bad == 0xC0123 && noname == 0xC0122 && over == 0xC0122 && missing == 0xC0124 &&
              !strncmp(got, "int main", 8),
          "*CC, *RunBox -- a failed compile, a source with no program name, a program over its "
          "source, a program not there: each refused, the source untouched",
          "&%X &%X &%X &%X", bad, noname, over, missing);

#endif

    snprintf(p, sizeof p, "%s/s", dir);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/s/tiny", dir);
    put(p, "        AREA    |Tiny$$Code|, CODE, READONLY\n"
           "        MOV     r0, #1\n"
           "        MOV     pc, lr\n"
           "        END\n");
    int ra = cli("RosAsm HostFS::BTTest.$.s.tiny -o HostFS::BTTest.$.tiny/o");
    snprintf(p, sizeof p, "%s/tiny.o", dir);
    if (getpid() == 1)
        check(ra == 0xC0125, "*RosAsm -- not in the box: its C is for the ROM, built where rosasm is",
              "&%X", ra);
    else
        check(!ra && access(p, R_OK) == 0, "*RosAsm -- the host's rosasm assembles s.tiny to tiny/o",
              "&%X", ra);

    /* *Mojo: the box's /usr/bin/mojo, an object for the box's ABI, roscc
     * with the Mojo runtime: an application beside the mojo directory,
     * run by its name.  Running an application from /init gives task 0
     * application space; it had none before, and is given it back after,
     * as the *CC checks above do, for the tasks' checks (#172). */
    if (getpid() == 1) {
        int mhad_slot = ros_slot_current != NULL;
        struct ros_environment menv;
        ros_env_save(&menv);
        snprintf(p, sizeof p, "%s/mojo", dir);
        mkdir(p, 0755);
        snprintf(p, sizeof p, "%s/mojo/hi", dir);
        put(p, "from riscos import os\n\ndef main():\n    os.write0(\"hello from *Mojo\\r\\n\")\n");
        snprintf(p, sizeof p, "%s/mojo/bad", dir);
        put(p, "def main():\n    undeclared()\n");
        int mj = cli("Mojo HostFS::BTTest.$.mojo.hi");
        snprintf(p, sizeof p, "%s/hi,ff8", dir);
        int mapp = access(p, F_OK) == 0;
        snprintf(p, sizeof p, "%s/hi.mojo.o", dir);
        int mtidy = access(p, F_OK) != 0;
        snprintf(p, sizeof p, "%s/hi.mojo.ll", dir);
        mtidy = mtidy && access(p, F_OK) != 0;
        int mbad = cli("Mojo HostFS::BTTest.$.mojo.bad");
        check(!mj && mapp && mtidy && mbad == 0xC0123,
              "*Mojo -- mojo.hi compiled by the box's mojo and linked by roscc with the Mojo runtime: the "
              "application hi beside mojo, typed &FF8, its object gone; mojo.bad refused, &C0123",
              "&%X, app %d, tidy %d, bad &%X", mj, mapp, mtidy, mbad);
        /* a source the host names two.mojo, given as two (HostFS's two/mojo
         * without its suffix): the compiler is given the .mojo file */
        snprintf(p, sizeof p, "%s/two.mojo", dir);
        put(p, "def main():\n    print(\"two\")\n");
        int mtwo = cli("Mojo HostFS::BTTest.$.two -o HostFS::BTTest.$.twoapp");
        snprintf(p, sizeof p, "%s/twoapp,ff8", dir);
        check(!mtwo && access(p, F_OK) == 0,
              "*Mojo -- two, whose file is two.mojo on the host, compiles to the application twoapp",
              "&%X", mtwo);
        /* the riscos package's library, and the runtime under it, run: files
         * and system (through its general SWI call), an error raised and
         * caught (the runtime's stack trace stub), sin (rostrt's maths), a
         * List of Strings and of structures and String of an Int64 (#173:
         * roscc mojo-ir), and the Territory Manager's date (#175) */
        snprintf(p, sizeof p, "%s/mojo/lib", dir);
        put(p, "from riscos import files, system\nfrom riscos.text import fixed\nfrom riscos.maths import power\n"
               "from std.math import sin, exp2\n\n"
               "@fieldwise_init\nstruct P(Copyable, Movable):\n    var x: Int\n    var y: Int\n    var z: Int\n\n"
               "def main() raises:\n"
               "    var s = String(\"sin \") + fixed(sin(Float64(system.monotonic_time() * 0) + 0.5), 4)\n"
               "    try:\n"
               "        _ = files.length(\"HostFS::BTTest.$.nothing\")\n"
               "    except e:\n"
               "        s += \" / \" + String(e)\n"
               "    var words = List[String]()\n"
               "    words.append(\"a\")\n"
               "    words.append(String(Int64(-1234567890123)))\n"
               "    var ps = List[P]()\n"
               "    for i in range(4):\n"
               "        ps.append(P(i, i * i, 7))\n"
               "    var t = 0\n"
               "    for p in ps:\n"
               "        t += p.x + p.y + p.z\n"
               "    s += \" / \" + String(\",\").join(words) + \" / \" + String(t)\n"
               "    s += \" / \" + String(system.date_text(\"%CE%YR\").byte_length())\n"
               "    s += \" / \" + String(1.0 / 3.0) + \" \" + String(2.0 ** 10) + \" \" + String(exp2(3.0))\n"
               "    s += \" \" + String(power(3.0, 2.0)) + \" \" + String(Float64(-0.0))\n"
               "    files.save_text(\"HostFS::BTTest.$.libout\", s)\n");
        int ml = cli("Mojo HostFS::BTTest.$.mojo.lib");
        snprintf(p, sizeof p, "%s/lib,ff8", dir);
        int mlapp = access(p, F_OK) == 0;
        int mhi = cli("HostFS::BTTest.$.hi");
        int mlrun = cli("HostFS::BTTest.$.lib");
        snprintf(p, sizeof p, "%s/libout", dir);
        int mthere = get(p, got, sizeof got);
        check(!ml && mlapp && !mhi && !mlrun && mthere &&
                  !strcmp(got, "sin 0.4794 / File 'HostFS::BTTest.$.nothing' not found / a,-1234567890123 / 48 / 4 / "
                               "0.3333333333333333 1024.0 8.0 9.0 -0.0"),
              "*Mojo -- mojo.lib, through the riscos package's files and system modules, compiles, links and "
              "runs: sin, a caught error, a List of Strings and of structures, String of an Int64, the date, "
              "Float64s written in full, ** and exp2 (#174); hi "
              "runs too",
              "&%X, app %d, hi &%X, run &%X, \"%s\"", ml, mlapp, mhi, mlrun, mthere ? got : "(no file)");
        if (!mhad_slot && ros_slot_current) {
            ros_task_resize_own(0);
            ros_env_load(&menv);
        }
        const char *mfiles[] = { "hi,ff8", "bad,ff8", "lib,ff8", "libout", "mojo/hi", "mojo/bad", "mojo/lib", "two.mojo", "twoapp,ff8",
                                 "mojo", NULL };
        for (int i = 0; mfiles[i]; i++) {
            snprintf(p, sizeof p, "%s/%s", dir, mfiles[i]);
            if (unlink(p))
                rmdir(p);
        }
    }

    ros_hostfs_unmount("BTTest");
    const char *files[] = { "hello,e1f", "out", "out2", "c/hello", "c/bad", "src/main", "s/tiny", "tiny.o", NULL };
    for (int i = 0; files[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        unlink(p);
    }
#if !defined(ROS_ARENA_HOSTED)
    const char *more[] = { "hello,ff8", "lhello,e1f", "greet,ff8", "out3", "bout,ffd", "bas/greet", NULL };
    for (int i = 0; more[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, more[i]);
        unlink(p);
    }
    snprintf(p, sizeof p, "%s/bas", dir);
    rmdir(p);
#endif
    const char *dirs[] = { "c", "src", "s", "", NULL };
    for (int i = 0; dirs[i]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, dirs[i]);
        rmdir(p);
    }
}
