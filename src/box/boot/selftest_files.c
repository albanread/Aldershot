/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_files.c: FileSwitch over HostFS and ResourceFS, through the
 * SWIs, against RISC OS's contracts (FileSys/FileSwitch) and the team's
 * HostFS mapping (vmchannel.c): a disc of the test's own, "Test", in a
 * fresh directory, looked at from both sides.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fileswitch.h"
#include "fsw.h"
#include "resourcefs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/environment.h"
#include "rosgd/heap.h"
#include "rosgd/module.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/task.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

static uint32_t text;               /* arena scratch: names at +0, +512; data at +1024 */
static char root[512];

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v ? 1 : s.c ? 2 : 0;       /* 1 error, 2 carry set */
}

static uint32_t errnum(const uint32_t r[8])
{
    return ((os_error *)ros_ptr(r[0]))->errnum;
}

static const char *errmess(const uint32_t r[8])
{
    return ((os_error *)ros_ptr(r[0]))->errmess;
}

static uint32_t name(const char *s)
{
    strcpy(ros_ptr(text), s);
    return text;
}

static uint32_t name2(const char *s)
{
    strcpy(ros_ptr(text + 512), s);
    return text + 512;
}

static int host_exists(const char *leaf)
{
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", root, leaf);
    return stat(p, &st) == 0;
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

static uint32_t file_info(const char *n, uint32_t r[8])
{
    uint32_t q[8] = { 20, name(n) };
    int v = swi(XOS_File, q);
    memcpy(r, q, sizeof q);
    return v == 1 ? 0xFFFFFFFFu : q[0];
}

static void streams(void)
{
    uint32_t r[8];
    uint32_t data = text + 1024;

    /* OpenIn of nothing: 0, or with &08 an error */
    uint32_t q[8] = { 0x40, name("HostFS::Test.$.Nothing") };
    int v = swi(XOS_Find, q);
    check(!v && q[0] == 0, "FileSwitch -- OpenIn of nothing is handle 0", "OpenIn of nothing is handle 0 (%u)", q[0]);
    uint32_t q2[8] = { 0x48, name("HostFS::Test.$.Nothing") };
    v = swi(XOS_Find, q2);
    check(v == 1 && errnum(q2) == 0xD6 && !strcmp(errmess(q2), "File 'HostFS::Test.$.Nothing' not found"),
          "FileSwitch -- OpenIn &48 of nothing", "OpenIn &48 of nothing: %s", v == 1 ? errmess(q2) : "no error");

    /* OS_File 5 of nothing: R0 0, and R2-R5 not given back as they came
     * in. 5.30's FileSwitch leaves its filing system's in them (R5 0).
     * Filer_Action reads a missing destination's R2 and R3 as its date
     * (its Newer test). With a source's date left there it copied nothing */
    uint32_t q5[8] = { 5, name("HostFS::Test.$.Nothing"), 0, 0xFFFFFF5Du, 0x216FA17Cu, 12, 3 };
    v = swi(XOS_File, q5);
    check(!v && q5[0] == 0 && q5[2] == 0 && q5[3] == 0 && q5[4] == 0 && q5[5] == 0,
          "FileSwitch -- OS_File 5 of nothing: R0 0, R2-R5 0, not the caller's",
          "OS_File 5 of nothing: v %d R0 %u R2 &%X R3 &%X R4 %u R5 &%X", v, q5[0], q5[2], q5[3], q5[4], q5[5]);

    /* OpenOut: a Data file, whose host name carries the type */
    uint32_t o[8] = { 0x80, name("HostFS::Test.$.Data/txt") };
    v = swi(XOS_Find, o);
    uint32_t h = o[0];
    check(!v && h == 255, "FileSwitch -- OpenOut gives handle 255", "OpenOut gives handle 255 (%u)", h);
    check(host_exists("Data.txt,ffd"), "HostFS -- OpenOut made Data.txt,ffd on the host", "OpenOut made Data.txt,ffd on the host");
    int ok = 1;
    for (const char *p = "abcdef"; *p; p++) {
        uint32_t b[8] = { (uint8_t)*p, h };
        ok &= swi(XOS_BPut, b) == 0;
    }
    uint32_t a[8] = { 2, h };
    swi(XOS_Args, a);
    check(ok && a[2] == 6, "FileSwitch -- BPut six bytes: extent 6", "BPut six bytes: extent %u", a[2]);
    uint32_t p1[8] = { 1, h, 2 };
    swi(XOS_Args, p1);
    uint32_t g[8] = { 0, h };
    v = swi(XOS_BGet, g);
    check(v == 0 && g[0] == 'c', "FileSwitch -- PTR 2, BGet gives 'c'", "PTR 2, BGet gives 'c' (%u)", g[0]);

    /* OS_GBPB: write at 6, read back from 1 */
    strcpy(ros_ptr(data), "XYZ");
    uint32_t w[8] = { 1, h, data, 3, 6 };
    v = swi(XOS_GBPB, w);
    check(!v && w[3] == 0 && w[4] == 9, "FileSwitch -- GBPB 1 writes at 6, PTR after", "GBPB 1 writes at 6: ptr %u", w[4]);
    memset(ros_ptr(data), 0, 16);
    uint32_t rd[8] = { 3, h, data, 16, 1 };
    v = swi(XOS_GBPB, rd);
    check(v == 2 && rd[3] == 8 && !memcmp(ros_ptr(data), "bcdefXYZ", 8), "FileSwitch -- GBPB 3 reads 8 of 16 from 1, C set",
          "GBPB 3 reads 8 of 16 from 1, C set (%u left)", rd[3]);

    /* EOF: C set once, then the error */
    uint32_t e1[8] = { 0, h };
    v = swi(XOS_BGet, e1);
    check(v == 2, "FileSwitch -- BGet at the end sets C", "BGet at the end sets C");
    uint32_t e2[8] = { 0, h };
    v = swi(XOS_BGet, e2);
    check(v == 1 && errnum(e2) == 0xDF, "FileSwitch -- BGet again: End of file", "BGet again: End of file (%s)",
          v == 1 ? errmess(e2) : "no error");
    uint32_t eof[8] = { 5, h };
    swi(XOS_Args, eof);
    check(eof[2] != 0, "FileSwitch -- OS_Args 5 says EOF", "OS_Args 5 says EOF");

    /* OS_Args 7: the canonical name */
    uint32_t cn[8] = { 7, h, data, 0, 0, 256 };
    v = swi(XOS_Args, cn);
    check(!v && !strcmp(ros_ptr(data), "HostFS::Test.$.Data/txt") && cn[5] == 256 - 24,
          "FileSwitch -- OS_Args 7: the canonical name", "OS_Args 7: %s", (char *)ros_ptr(data));

    /* Open for update again: refused while open */
    uint32_t up[8] = { 0xC0, name("HostFS::Test.$.Data/txt") };
    v = swi(XOS_Find, up);
    check(v == 1 && errnum(up) == 0x1DCC2, "HostFS -- a second open while open for update", "a second open while open for update: %s",
          v == 1 ? errmess(up) : "opened");
    uint32_t c[8] = { 0, h };
    check(swi(XOS_Find, c) == 0, "FileSwitch -- close", "close");

    /* OpenIn: read only */
    uint32_t in[8] = { 0x40, name("HostFS::Test.$.data/TXT") };
    v = swi(XOS_Find, in);
    h = in[0];
    check(!v && h, "FileSwitch -- OpenIn, the name in another case", "OpenIn, the name in another case");
    uint32_t bp[8] = { 'x', h };
    v = swi(XOS_BPut, bp);
    check(v == 1 && errnum(bp) == 0xC1, "FileSwitch -- BPut to OpenIn: Not open for update", "BPut to OpenIn: Not open for update");
    uint32_t past[8] = { 1, h, 100 };
    v = swi(XOS_Args, past);
    check(v == 1 && errnum(past) == 0xB7, "FileSwitch -- PTR past the end, read only: Outside file", "PTR past the end, read only: Outside file");
    uint32_t info[8] = { 0xFE, h };
    swi(XOS_Args, info);
    check((info[0] & 0xC0) == 0x40 && info[2] == 220, "FileSwitch -- OS_Args &FE: read, HostFS's number",
          "OS_Args &FE: status &%X, FS %u", info[0], info[2]);
    uint32_t cl[8] = { 0, 0 };
    swi(XOS_Find, cl);
    uint32_t bad[8] = { 0, h };
    v = swi(XOS_BGet, bad);
    check(v == 1 && errnum(bad) == 0xDE, "FileSwitch -- BGet on a closed handle: Channel", "BGet on a closed handle: Channel");
    uint32_t unal[8] = { 0xFE, h };
    v = swi(XOS_Args, unal);
    check(!v && unal[0] == 0x800 && unal[2] == 0, "FileSwitch -- OS_Args &FE unallocated", "OS_Args &FE unallocated");
    (void)r;
}

static void put_host(const char *leaf, const char *data);

static void files(void)
{
    uint32_t r[8];
    uint32_t data = text + 1024;

    /* A directory carries no load or exec address, as the team's HostFS
     * answers (5.30's Full info: 00000000 00000000), read alone (OS_File
     * 17) or listed (OS_GBPB 10) */
    uint32_t dmk[8] = { 8, name("HostFS::Test.$.DirInfo") };
    int vdmk = swi(XOS_File, dmk);
    uint32_t di[8] = { 17, name("HostFS::Test.$.DirInfo") };
    int vdi = swi(XOS_File, di);
    uint32_t gl[8] = { 10, name("HostFS::Test.$"), data, 64, 0, 256, name2("DirInfo") };
    int vgl = swi(XOS_GBPB, gl);
    uint32_t dload = ros_ld32(data), dexec = ros_ld32(data + 4), dtype = ros_ld32(data + 16);
    uint32_t drm[8] = { 6, name("HostFS::Test.$.DirInfo") };
    swi(XOS_File, drm);
    check(!vdmk && !vdi && di[0] == 2 && di[2] == 0 && di[3] == 0 && di[4] == 0 && !vgl &&
              gl[3] == 1 && dtype == 2 && dload == 0 && dexec == 0,
          "HostFS -- a directory's load and exec addresses 0, read and listed, as the team's "
          "HostFS gives them",
          "OS_File 17: %u &%08X &%08X %u; OS_GBPB 10: %u read, type %u, &%08X &%08X", di[0], di[2],
          di[3], di[4], gl[3], dtype, dload, dexec);

    /* Save typed: Text needs no decoration, a type set renames */
    strcpy(ros_ptr(data), "hello world");
    uint32_t s[8] = { 10, name("HostFS::Test.$.Hello"), 0xFFF, 0, data, data + 11 };
    int v = swi(XOS_File, s);
    check(!v && host_exists("Hello"), "HostFS -- OS_File 10 Text: Hello on the host", "OS_File 10 Text: Hello on the host");
    uint32_t t = file_info("HostFS::Test.$.Hello", r);
    check(t == 1 && r[4] == 11 && r[6] == 0xFFF && r[5] == 3, "FileSwitch -- OS_File 20: a Text file, its length and attributes",
          "OS_File 20: file, length %u, type &%03X, attr &%X", r[4], r[6], r[5]);
    uint32_t st[8] = { 18, name("HostFS::Test.$.Hello"), 0xFFB };
    v = swi(XOS_File, st);
    check(!v && host_exists("Hello,ffb") && !host_exists("Hello"), "HostFS -- OS_File 18 &FFB renames to Hello,ffb",
          "OS_File 18 &FFB renames to Hello,ffb");
    t = file_info("HostFS::Test.$.hello", r);
    check(t == 1 && r[6] == 0xFFB, "FileSwitch -- type &FFB, the name in another case", "type &%03X by another case", r[6]);

    /* Load */
    memset(ros_ptr(data), 0, 32);
    uint32_t ld[8] = { 255, name("HostFS::Test.$.Hello"), data, 0 };
    v = swi(XOS_File, ld);
    check(!v && !memcmp(ros_ptr(data), "hello world", 11) && ld[4] == 11, "FileSwitch -- OS_File 255 loads it",
          "OS_File 255 loads it");
    uint32_t ldn[8] = { 255, name("HostFS::Test.$.Nope"), data, 0 };
    v = swi(XOS_File, ldn);
    check(v == 1 && errnum(ldn) == 0xD6, "FileSwitch -- load of nothing", "load of nothing: %s",
          v == 1 ? errmess(ldn) : "no error");

    /* Directories, and a listing */
    uint32_t md[8] = { 8, name("HostFS::Test.$.Dir") };
    v = swi(XOS_File, md);
    check(!v && host_exists("Dir"), "HostFS -- OS_File 8 makes Dir", "OS_File 8 makes Dir");
    uint32_t sv[8] = { 10, name("HostFS::Test.$.Dir.File"), 0xFFD, 0, data, data + 4 };
    v = swi(XOS_File, sv);
    check(!v && host_exists("Dir/File,ffd"), "HostFS -- a Data file in Dir: File,ffd", "a Data file in Dir: File,ffd");
    uint32_t ld2[8] = { 255, name("HostFS::Test.$.Dir"), data, 0 };
    v = swi(XOS_File, ld2);
    check(v == 1 && errnum(ld2) == 0xA8, "FileSwitch -- load of a directory", "load of a directory: %s",
          v == 1 ? errmess(ld2) : "no error");
    memset(ros_ptr(data), 0, 256);
    uint32_t ls[8] = { 10, name("HostFS::Test.$"), data, 10, 0, 256, 0 };
    v = swi(XOS_GBPB, ls);
    const char *n1 = ros_ptr(data + 20);
    uint32_t len1 = ((20 + (uint32_t)strlen(n1) + 1) + 3) & ~3u;
    const char *n2 = ros_ptr(data + len1 + 20);
    check(!v && ls[3] == 3 && ls[4] == 0xFFFFFFFFu && !strcmp(n1, "Data/txt") &&
              !strcmp(n2, "Dir") && ros_ld32(data + len1 + 16) == 2,
          "FileSwitch -- OS_GBPB 10 lists $, sorted, with types", "OS_GBPB 10: %u entries, %s, %s", ls[3], n1, n2);
    uint32_t lw[8] = { 9, name("HostFS::Test.$"), data, 10, 0, 256, name2("H*") };
    v = swi(XOS_GBPB, lw);
    check(!v && lw[3] == 1 && !strcmp(ros_ptr(data), "Hello"), "FileSwitch -- OS_GBPB 9 with the wildcard H*",
          "OS_GBPB 9 with H*: %s", (char *)ros_ptr(data));

    /* *Dir, relative names, ^, canonicalising */
    uint32_t cd0[8] = { 0, name("HostFS::Test.$") };
    swi(XOS_FSControl, cd0);
    uint32_t cd[8] = { 0, name("HostFS::Test.$.Dir") };
    v = swi(XOS_FSControl, cd);
    t = file_info("File", r);
    check(!v && t == 1 && r[6] == 0xFFD, "FileSwitch -- *Dir Dir, then File is there", "*Dir Dir, then File is there");
    uint32_t can[8] = { 37, name("^.He*"), data, 0, 0, 256 };
    v = swi(XOS_FSControl, can);
    check(!v && !strcmp(ros_ptr(data), "HostFS::Test.$.Hello"), "FileSwitch -- canonicalise ^.He*",
          "canonicalise ^.He*: %s", (char *)ros_ptr(data));
    uint32_t can2[8] = { 37, name("@"), 0, 0, 0, 0 };
    swi(XOS_FSControl, can2);
    check(can2[5] == (uint32_t)-(int)(strlen("HostFS::Test.$.Dir") + 1), "FileSwitch -- canonicalise with no buffer sizes it",
          "canonicalise, sizing: R5 %d", (int)can2[5]);
    uint32_t bk[8] = { 40 };
    swi(XOS_FSControl, bk);
    t = file_info("Hello", r);
    check(t == 1, "FileSwitch -- *Back: Hello is in the CSD again", "*Back: Hello is in the CSD again");

    /* Wildcards: read through them, never write */
    t = file_info("HostFS::Test.$.He#lo", r);
    check(t == 1, "FileSwitch -- He#lo matches Hello", "He#lo matches Hello");
    uint32_t sw[8] = { 10, name("HostFS::Test.$.He*"), 0xFFF, 0, data, data };
    v = swi(XOS_File, sw);
    check(v == 1 && errnum(sw) == 0xFD, "FileSwitch -- save to He*", "save to He*: %s",
          v == 1 ? errmess(sw) : "no error");

    /* A path variable */
    uint32_t sv2[8] = { name("Test$Path"), name2("HostFS::Test.$.Nope.,HostFS::Test.$.Dir."),
                        (uint32_t)strlen("HostFS::Test.$.Nope.,HostFS::Test.$.Dir."), 0, 0 };
    swi(XOS_SetVarVal, sv2);
    t = file_info("Test:File", r);
    check(t == 1, "FileSwitch -- Test:File through Test$Path's second place", "Test:File through Test$Path's second place");
    uint32_t un[8] = { name("Test$Path"), 0, 0xFFFFFFFFu, 0, 0 };
    swi(XOS_SetVarVal, un);

    /* Rename, locking, delete */
    uint32_t rn[8] = { 25, name("HostFS::Test.$.Hello"), name2("HostFS::Test.$.Dir.Moved") };
    v = swi(XOS_FSControl, rn);
    check(!v && host_exists("Dir/Moved,ffb"), "HostFS -- rename keeps the type: Dir/Moved,ffb", "rename keeps the type: Dir/Moved,ffb");
    uint32_t rn2[8] = { 25, name("HostFS::Test.$.Dir"), name2("HostFS::Test.$.Dir.Sub") };
    v = swi(XOS_FSControl, rn2);
    check(v == 1 && errnum(rn2) == 0xB0, "FileSwitch -- a directory into itself: Bad rename", "a directory into itself: Bad rename");
    uint32_t lk[8] = { 4, name("HostFS::Test.$.Dir.Moved"), 0, 0, 0, 0x0B };
    swi(XOS_File, lk);
    uint32_t dl[8] = { 6, name("HostFS::Test.$.Dir.Moved") };
    v = swi(XOS_File, dl);
    check(v == 1 && errnum(dl) == 0x1DCC3, "HostFS -- delete of a locked file", "delete of a locked file: %s",
          v == 1 ? errmess(dl) : "deleted");
    uint32_t ul[8] = { 4, name("HostFS::Test.$.Dir.Moved"), 0, 0, 0, 0x03 };
    swi(XOS_File, ul);
    uint32_t dd[8] = { 6, name("HostFS::Test.$.Dir") };
    v = swi(XOS_File, dd);
    check(v == 1 && errnum(dd) == 0x1DCB4, "HostFS -- delete of a full directory", "delete of a full directory: %s",
          v == 1 ? errmess(dd) : "deleted");
    uint32_t d2[8] = { 6, name("HostFS::Test.$.Dir.Moved") };
    v = swi(XOS_File, d2);
    check(!v && d2[0] == 1 && !host_exists("Dir/Moved,ffb"), "FileSwitch -- delete returns what it was",
          "delete returns what it was: type %u", d2[0]);
    uint32_t d3[8] = { 6, name("HostFS::Test.$.Dir.Moved") };
    v = swi(XOS_File, d3);
    check(!v && d3[0] == 0, "FileSwitch -- delete of nothing: R0 0, no error", "delete of nothing: R0 0, no error");

    /* What the HostFS suite found (tests/hostfs/rosgd_suite.py) */
    uint32_t fs49[8] = { 49, name("HostFS::Test.$") }, fs55[8] = { 55, name("HostFS::Test.$") };
    int f1 = swi(XOS_FSControl, fs49) == 0 && fs49[2] != 0 && swi(XOS_FSControl, fs55) == 0 &&
             (fs55[3] | fs55[4]) != 0;
    check(f1, "FileSwitch -- OS_FSControl 49 and 55, free space", NULL);
    put_host("lower", "x");
    uint32_t cr[8] = { 25, name("HostFS::Test.$.lower"), name2("HostFS::Test.$.LOWER") };
    int f2 = swi(XOS_FSControl, cr) == 0;
    DIR *hd = opendir(root);
    int upper_seen = 0;
    for (struct dirent *de; hd && (de = readdir(hd)) != NULL;)
        upper_seen |= !strcmp(de->d_name, "LOWER");
    if (hd)
        closedir(hd);
    check(f2 && upper_seen, "HostFS -- a rename that only changes case changes the host's name",
          NULL);
    put_host("amb", "1");
    put_host("amb,ffb", "22");
    put_host("cafe\xCC\x81", "nfd");          /* "cafe" and a combining acute */
    uint32_t am[8] = { 9, name("HostFS::Test.$"), data, 100, 0, 1000, name2("amb") };
    swi(XOS_GBPB, am);
    t = file_info("HostFS::Test.$.caf\xE9", r);
    check(am[3] == 1 && t == 1 && r[4] == 3,
          "HostFS -- two host files of one RISC OS name list once; a decomposed host name composes",
          "%u amb, caf\xE9 %u", am[3], t);
    remove_tree((snprintf((char *)ros_ptr(data), 256, "%s/amb", root), (char *)ros_ptr(data)));
    remove_tree((snprintf((char *)ros_ptr(data), 256, "%s/amb,ffb", root), (char *)ros_ptr(data)));
    remove_tree((snprintf((char *)ros_ptr(data), 256, "%s/cafe\xCC\x81", root), (char *)ros_ptr(data)));
    remove_tree((snprintf((char *)ros_ptr(data), 256, "%s/LOWER", root), (char *)ros_ptr(data)));
    put_host("gone", "abcdef");
    uint32_t go[8] = { 0x40, name("HostFS::Test.$.gone") };
    swi(XOS_Find, go);
    remove_tree((snprintf((char *)ros_ptr(data), 256, "%s/gone", root), (char *)ros_ptr(data)));
    uint32_t gg[8] = { 4, go[0], data, 3 };
    int gv = swi(XOS_GBPB, gg);
    uint32_t gc[8] = { 0, go[0] };
    int gcv = swi(XOS_Find, gc);
    check(go[0] && gv == 1 && (errnum(gg) & 0xFF) == 0xD6 && gcv == 0,
          "HostFS -- a file deleted on the host under an open handle: Not found, and it closes",
          NULL);

    /* Host names RISC OS cannot spell as they are */
    FILE *f = fopen((snprintf((char *)ros_ptr(data), 256, "%s/a.b c", root), (char *)ros_ptr(data)), "w");
    if (f)
        fclose(f);
    t = file_info("HostFS::Test.$.a/b\xA0" "c", r);
    check(t == 1, "HostFS -- host 'a.b c' is 'a/b<hard space>c'", "host 'a.b c' is 'a/b<hard space>c'");
}

/* MessageTrans reads a file that is not in ResourceFS through OS_File */
static void messages(void)
{
    uint32_t data = text + 1024;
    strcpy(ros_ptr(data), "Greet:Hello from HostFS\n");
    uint32_t sv[8] = { 10, name("HostFS::Test.$.Msgs"), 0xFFF, 0, data, data + 24 };
    int v = swi(XOS_File, sv);
    uint32_t desc = data + 256;
    uint32_t op[8] = { desc, name2("HostFS::Test.$.Msgs"), 0 };
    v |= swi(XMessageTrans_OpenFile, op);
    uint32_t lk[8] = { desc, name("Greet"), 0, 0 };
    int lv = swi(XMessageTrans_Lookup, lk);
    check(!v && lv != 1 && lk[3] == 17 && !memcmp(ros_ptr(lk[2]), "Hello from HostFS", 17),
          "MessageTrans -- a Messages file on HostFS, through OS_File", "%s",
          lv == 1 ? errmess(lk) : v ? "not opened" : "wrong text");
    uint32_t cl[8] = { desc };
    swi(XMessageTrans_CloseFile, cl);
}

static void resources(void)
{
    uint32_t r[8];
    uint32_t data = text + 1024;
    uint32_t t = file_info("Resources:$.Resources.Global.Messages", r);
    check(t == 1, "ResourceFS -- Resources:$.Resources.Global.Messages is a file", "Resources:$.Resources.Global.Messages is a file");
    t = file_info("Resources:$.Resources", r);
    check(t == 2 && r[6] == 0x1000, "ResourceFS -- Resources:$.Resources is a directory", "Resources:$.Resources is a directory");
    uint32_t o[8] = { 0x4C, name("Resources:$.Resources.Global.Messages") };
    int v = swi(XOS_Find, o);
    uint32_t h = o[0];
    uint32_t fh[8] = { 21, h };
    swi(XOS_FSControl, fh);
    check(!v && fh[1] == ros_resourcefs_find("Resources:$.Resources.Global.Messages") + 4 &&
              fh[2] == 0x2E,
          "ResourceFS -- ReadFSHandle is the file's data, as RISC OS's", "ReadFSHandle is not the data");
    uint32_t g[8] = { 4, h, data, 1 };
    swi(XOS_GBPB, g);
    check(g[3] == 0, "ResourceFS -- read a byte", "read a byte");
    uint32_t c[8] = { 0, h };
    swi(XOS_Find, c);
    uint32_t sv[8] = { 10, name("Resources:$.New"), 0xFFF, 0, data, data };
    v = swi(XOS_File, sv);
    check(v == 1, "ResourceFS -- a save is refused", "a save is refused: %s", v == 1 ? errmess(sv) : "saved");
    uint32_t ls[8] = { 9, name("Resources:$.Resources"), data, 1, 0, 256, 0 };
    v = swi(XOS_GBPB, ls);
    check(!v && ls[3] == 1, "ResourceFS -- the first entry of $.Resources", "the first entry of $.Resources: %s",
          (char *)ros_ptr(data));
}

/* ---- the utilities, through the commands, their output caught ---- */

#define WRCHV 0x03u
static char out[4096];
static unsigned outn;
static char last_error[256];
static int pass_on;                     /* let the default owner have it too */

static int wrch(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (outn < sizeof out - 1)
        out[outn++] = (char)s->r[0];
    out[outn] = 0;
    return pass_on ? ROS_VECTOR_PASS : ROS_VECTOR_CLAIM;
}

/* OS_CLI: gives the error number, or 0.  The output is left in out */
static uint32_t cli(const char *cmd)
{
    strcpy(ros_ptr(text), cmd);
    outn = 0, out[0] = 0;
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = text;
    ros_swi(&s, XOS_CLI);
    snprintf(last_error, sizeof last_error, "%s",
             s.v ? ((os_error *)ros_ptr(s.r[0]))->errmess : "");
    return s.v ? ((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

static int says(const char *cmd, const char *want)
{
    uint32_t e = cli(cmd);
    int ok = e == 0 && strcmp(out, want) == 0;
    if (!ok)
        ros_console_printf("        %s: &%X %s \"%s\"\n", cmd, e, last_error, out);
    return ok;
}

static int says_part(const char *cmd, const char *part)
{
    uint32_t e = cli(cmd);
    int ok = e == 0 && strstr(out, part) != NULL;
    if (!ok)
        ros_console_printf("        %s: &%X \"%s\"\n", cmd, e, out);
    return ok;
}

static int fails(const char *cmd, uint32_t errnum)
{
    uint32_t e = cli(cmd);
    if (e != errnum)
        ros_console_printf("        %s: &%X, want &%X \"%s\"\n", cmd, e, errnum, out);
    return e == errnum;
}

static int host_is(const char *leaf, const char *data)
{
    char p[1024], b[256];
    snprintf(p, sizeof p, "%s/%s", root, leaf);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strcmp(b, data) == 0;
}

static void put_host(const char *leaf, const char *data)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, leaf);
    FILE *f = fopen(p, "wb");
    if (f) {
        fputs(data, f);
        fclose(f);
    }
}

static void utilities(void)
{
    uint32_t data = text + 1024;
    setenv("TZ", "UTC", 1);             /* the box's clock is UTC; hosted, pin it */
    tzset();
    ros_vector_claim_native(WRCHV, wrch, 0);

    /* OS_ConvertDateAndTime: 12:34:56.78, 1 January 2000, UTC */
    ros_st32(data, 0x798CE60E);
    ros_st8(data + 4, 0x49);
    strcpy(ros_ptr(data + 16), "%W3 %DY%ST %MO %CE%YR %24:%MI:%SE.%CS %ZMN/%DN %12%AM");
    uint32_t cd[8] = { data, data + 128, 100, data + 16 };
    int v = swi(XOS_ConvertDateAndTime, cd);
    check(!v && !strcmp(ros_ptr(data + 128), "Sat 01st January 2000 12:34:56.78 1/001 12pm") &&
              cd[1] == data + 128 + 44 && cd[2] == 100 - 45,
          "OS_ConvertDateAndTime -- the UK territory's codes, %Z, R1 the terminator, which "
          "takes a byte of R2", "%s",
          (char *)ros_ptr(data + 128));

    /* A Text file of a known date, then the listings: whole seconds, which
     * a 9p share keeps (it drops the centiseconds) */
    put_host("Hello", "hello world");
    uint32_t w[8] = { 1, name("HostFS::Test.$.Hello"), 0xFFFFFF49u, 0x798CE5C0u, 0, 3 };
    swi(XOS_File, w);
    int l1 = says("FileInfo HostFS::Test.$.Hello",
                  "Hello        WR/     Text      12:34:56.00 01-Jan-2000 0000000B\n\r") &&
             says_part("Info HostFS::Test.$.Hello",
                       "Hello        WR/     Text      12:34:56 01-Jan-2000 ") &&
             says_part("Ex HostFS::Test.$", "Dir. HostFS::Test.$ Option 00 (Off)\n\r") &&
             says_part("Ex HostFS::Test.$", "Lib. \"Unset\"\n\rURD  \"Unset\"\n\r") &&
             says_part("Cat HostFS::Test.$", "Hello        WR/   ");
    check(l1, "*FileInfo, *Info, *Ex, *Cat -- FileSwitch's layouts, dates, the title block",
          NULL);

    /* *Access */
    uint32_t r[8];
    int a1 = cli("Access HostFS::Test.$.Hello LR/r") == 0 && file_info("HostFS::Test.$.Hello", r) == 1 &&
             r[5] == 0x19 && fails("Access HostFS::Test.$.Hello WX", 0x1EA) &&
             cli("Access HostFS::Test.$.Hel* WR") == 0 &&
             file_info("HostFS::Test.$.Hello", r) == 1 && r[5] == 0x03;
    check(a1, "*Access -- letters, public after /, wildcards; bad letters &1EA", NULL);

    /* *SetType, so OS_FSControl 31: a type's name, looked up among the
     * File$Type_* variables as FileSwitch does it (FSCtrl2,
     * FileTypeFromStringEntry). Either case is accepted, and a name with a
     * space in it, or a number, with or without &. A name that is none of
     * them gives "File type is unrecognised" */
    int y1 = cli("SetType HostFS::Test.$.Hello Obey") == 0 &&
             file_info("HostFS::Test.$.Hello", r) == 1 && r[6] == 0xFEB &&
             cli("SetType HostFS::Test.$.Hello bbc rom") == 0 &&
             file_info("HostFS::Test.$.Hello", r) == 1 && r[6] == 0xBBC &&
             cli("SetType HostFS::Test.$.Hello FFD") == 0 &&
             file_info("HostFS::Test.$.Hello", r) == 1 && r[6] == 0xFFD &&
             cli("SetType HostFS::Test.$.Hello &FF9") == 0 &&
             file_info("HostFS::Test.$.Hello", r) == 1 && r[6] == 0xFF9 &&
             fails("SetType HostFS::Test.$.Hello Nonsuch", 0x41D);
    check(y1, "*SetType -- a type by name, without case and with a space in it; a number, "
              "with & or without; a name that is not a type &41D", NULL);
    /* the file as the listings had it: Text, and its date */
    swi(XOS_File, (uint32_t[8]){ 1, name("HostFS::Test.$.Hello"), 0xFFFFFF49u, 0x798CE5C0u, 0, 3 });

    /* *Copy: one file, then over it only with F, newer with N */
    int c1 = cli("Copy HostFS::Test.$.Hello HostFS::Test.$.Hello2 ~C~V") == 0 &&
             host_is("Hello2", "hello world") && file_info("HostFS::Test.$.Hello2", r) == 1 &&
             r[2] == 0xFFFFFF49u && r[3] == 0x798CE5C0u;
    put_host("Hello2", "changed");
    swi(XOS_File, (uint32_t[8]){ 1, name("HostFS::Test.$.Hello2"), 0xFFFFFF49u, 0x798CE5C0u, 0, 3 });
    c1 = c1 && cli("Copy HostFS::Test.$.Hello HostFS::Test.$.Hello2 ~C~V") == 0 &&
         host_is("Hello2", "changed") &&
         cli("Copy HostFS::Test.$.Hello HostFS::Test.$.Hello2 ~C~VFN") == 0 &&
         host_is("Hello2", "changed") &&
         cli("Copy HostFS::Test.$.Hello HostFS::Test.$.Hello2 ~C~VF") == 0 &&
         host_is("Hello2", "hello world") &&
         says("Copy HostFS::Test.$.Hello HostFS::Test.$.Hello3 ~C V",
              "File HostFS::Test.$.Hello copied as HostFS::Test.$.Hello3, 11 bytes\n\r"
              "1 file copied, total 11 bytes\n\r") &&
         fails("Copy HostFS::Test.$.Hello HostFS::Test.$.x.a* ~C", 0xB1) &&
         fails("Copy HostFS::Test.$.Nope* HostFS::Test.$.* ~C", 0x415);
    check(c1, "*Copy -- dates and types kept; existing files only with F, N by date; V's "
              "report; &B1, &415", NULL);

    /* A tree: copied with R, counted, wiped */
    swi(XOS_File, (uint32_t[8]){ 8, name("HostFS::Test.$.Tree") });
    swi(XOS_File, (uint32_t[8]){ 8, name("HostFS::Test.$.Tree.Sub") });
    put_host("Tree/a", "12345");
    put_host("Tree/Sub/b", "123");
    swi(XOS_File, (uint32_t[8]){ 8, name("HostFS::Test.$.Copy") });
    uint32_t cn[8] = { 28, name("HostFS::Test.$.Tree"), 0, 1 };
    int t1 = cli("Copy HostFS::Test.$.Tree.* HostFS::Test.$.Copy.* R~C~V") == 0 &&
             host_is("Copy/a", "12345") && host_is("Copy/Sub/b", "123") &&
             swi(XOS_FSControl, cn) == 0 && cn[2] == 8 && cn[3] == 2 &&
             says("Count HostFS::Test.$.Copy", "2 files counted, total 8 bytes\n\r") &&
             cli("Wipe HostFS::Test.$.Copy.* ~C~VR") == 0 && !host_exists("Copy/Sub") &&
             !host_exists("Copy/a") && fails("Wipe HostFS::Test.$.Copy.* ~C", 0x416) &&
             says("Wipe HostFS::Test.$.Hello3 ~C V",
                  "File HostFS::Test.$.Hello3 deleted\n\r1 file deleted\n\r");
    check(t1, "*Copy R, *Count, *Wipe R -- a tree copied, counted, deleted; &416", NULL);

    /* The kernel's file commands */
    ros_st32(data, 0x64636261);             /* "abcde" */
    ros_st8(data + 4, 'e');
    char cmd[200];
    snprintf(cmd, sizeof cmd, "Save HostFS::Test.$.S %X +5", data);
    int k1 = cli(cmd) == 0 && host_is("S,ffd", "abcde");
    memset(ros_ptr(data + 16), 0, 8);
    snprintf(cmd, sizeof cmd, "Load HostFS::Test.$.S %X", data + 16);
    k1 = k1 && cli(cmd) == 0 && !memcmp(ros_ptr(data + 16), "abcde", 5) &&
         cli("Create HostFS::Test.$.C 10") == 0 && file_info("HostFS::Test.$.C", r) == 1 &&
         r[4] == 16 && r[6] == 0xFFD && cli("Delete HostFS::Test.$.C") == 0 &&
         fails("Delete HostFS::Test.$.C", 0xD6) && cli("Remove HostFS::Test.$.C") == 0;
    check(k1, "*Save, *Load, *Create, *Delete, *Remove -- hex addresses, +length; Delete "
              "complains, Remove does not", NULL);
    put_host("T", "a\tb\r\nc|\x81");
    int k2 = says("Type HostFS::Test.$.T", "a|Ib\n\rc|||!|A\n\r") &&
             says("List HostFS::Test.$.T", "   1 a|Ib\n\r   2 c|||!|A\n\r") &&
             says("Type -File HostFS::Test.$.T -TabExpand", "a        b\n\rc|||!|A\n\r") &&
             says("Print HostFS::Test.$.T", "a\tb\r\nc|\x81") &&
             says_part("Dump HostFS::Test.$.S", "Address  : 00 01 02 03") &&
             says_part("Dump HostFS::Test.$.S",
                       "00000000 : 61 62 63 64 65                                  : abcde\n\r");
    check(k2, "*Type, *List, *Print, *Dump -- GSREAD format, line ends, numbers, tabs, the dump",
          NULL);

    /* Where characters go: *Spool, redirection, *Exec, OS_ReadLine */
    pass_on = 1;
    int s1 = cli("Spool HostFS::Test.$.Sp") == 0 && cli("Echo spooled") == 0 &&
             !strcmp(out, "spooled\n\r") && cli("Spool") == 0 &&
             host_is("Sp,ffd", "spooled\n\r") &&
             cli("SpoolOn HostFS::Test.$.Sp") == 0 && cli("Echo more") == 0 &&
             cli("Spool") == 0 && host_is("Sp,ffd", "spooled\n\rmore\n\r");
    int s2 = says("Echo hidden { > HostFS::Test.$.R } after", "") &&
             host_is("R,ffd", "hidden after\n\r") &&
             says("Echo again { >> HostFS::Test.$.R }", "") &&
             host_is("R,ffd", "hidden after\n\ragain \n\r") &&
             says("Echo seen", "seen\n\r");
    put_host("In", "typed\rline two\r");
    uint32_t rl[8] = { data, 100, ' ', 255, 0 };
    uint32_t e1 = cli("Exec HostFS::Test.$.In");
    outn = 0, out[0] = 0;
    int rv1 = swi(XOS_ReadLine32, rl);
    int s3 = e1 == 0 && rv1 == 0 && rl[1] == 5 && !memcmp(ros_ptr(data), "typed\r", 6) &&
             !strcmp(out, "typed\n\r");
    uint32_t rl2[8] = { data, 4, ' ', 255, 0 };
    outn = 0, out[0] = 0;
    int rv2 = swi(XOS_ReadLine32, rl2);
    s3 = s3 && rv2 == 0 && rl2[1] == 4 && !memcmp(ros_ptr(data), "line", 4) &&
         !strcmp(out, "line\x07\x07\x07\x07\n\r");
    uint32_t b198[8] = { 198, 0, 0xFF };
    swi(XOS_Byte, b198);
    /* The file stays open until a read past its end, as the kernel's does.  Only *Exec closes it */
    s3 = s3 && b198[1] != 0 && cli("Exec") == 0;
    uint32_t c198[8] = { 198, 0, 0xFF };
    swi(XOS_Byte, c198);
    s3 = s3 && c198[1] == 0;
    pass_on = 0;
    check(s1 && s2 && s3,
          "*Spool, *SpoolOn, redirection, *Exec, OS_ReadLine32 -- output copied, sent "
          "elsewhere, input from a file, a line read and kept to its length", "%u %u %u", s1, s2, s3);

    /* A name in a module's ROM image is readable (T0Demo's title).  The ROM
     * is not writable */
    uint32_t title = 0xFC010000u + ros_ld32(0xFC010000u + 0x10);
    uint32_t rn[8] = { 5, title };
    int rv = swi(XOS_File, rn);
    check(rv == 0 && rn[0] == 0 && ros_arena_readable(title, title + 4) &&
              !ros_arena_valid(title, title + 4),
          "FileSwitch -- a name in the ROM image is read; the ROM is readable, not writable",
          "%s", rv ? errmess(rn) : "");

    /* OS_WriteS as the compiler calls it: through WrchV */
    strcpy(ros_ptr(data), "inline string");
    outn = 0, out[0] = 0;
    struct ros_cpu ws;
    ros_cpu_enter(&ws);
    ros_writes(&ws, data);
    check(!strcmp(out, "inline string") && !ws.v,
          "ros_writes -- OS_WriteS's inline string, through WrchV", "\"%s\"", out);

    /* Running: a Text file is *Typed, through Alias$@RunType_FFF */
    int n1 = says("Run HostFS::Test.$.Hello", "hello world\n\r") &&
             says("HostFS::Test.$.Hello", "hello world\n\r") &&
             says("/HostFS::Test.$.Hello", "hello world\n\r") &&
             fails("Nosuch", 0xD6) && fails("Run HostFS::Test.$.Tree", 0xA8) &&
             says_part("-Resources-Cat", "Dir. Resources:$ Option") &&
             says_part("Resources:Cat", "Dir. Resources:$ Option") &&
             says_part("Cat", "Dir. HostFS::Test.$ Option");
    check(n1, "Running files -- *Run, by name, /; Run$Path; not found &D6; \"fs:\" and "
              "\"-fs-\" the temporary filing system for one command", NULL);

    /* An application directory runs its !Run (FileSwitch's TryPlingRun) */
    char app[600];
    snprintf(app, sizeof app, "%s/!App", root);
    mkdir(app, 0777);
    snprintf(app, sizeof app, "%s/!App/!Run,feb", root);
    FILE *af = fopen(app, "w");
    if (af) {
        fputs("Echo app ran <Obey$Dir> %0\n", af);
        fclose(af);
    }
    check(says_part("Run HostFS::Test.$.!App one", "app ran HostFS::Test.$.!App one") &&
          fails("Run HostFS::Test.$.Tree", 0xA8),
          "Running files -- a directory runs its !Run, Obey$Dir the directory, with the "
          "arguments; one without is a directory, &A8", "%s", out);

    /* The command tail starts after the name as written: a <Var> naming the
     * file (TaskWindow's %Run <TaskWindow$Server>) passes no arguments */
    int vt = cli("Set SelfTest$App HostFS::Test.$.!App") == 0 &&
             says("Run <SelfTest$App>", "app ran HostFS::Test.$.!App \n\r") &&
             says("%Run <SelfTest$App> two", "app ran HostFS::Test.$.!App two\n\r") &&
             says("/<SelfTest$App>", "app ran HostFS::Test.$.!App \n\r");
    cli("Unset SelfTest$App");
    check(vt, "Running files -- a name from a <Var>: the arguments are the line's after the "
              "name as written, none for Run <Var>", "%s", out);

    /* An Obey file's last line is a tail call: 80 files, each obeying the
     * next on its last line, run seven times (as an !Run's last line
     * starts its application each time) and leave no level behind.
     * Files also nest 80 deep where a line follows (N1..N80). That is as
     * deep as the stacks allow (recursion(), below), with no count of
     * levels */
    char cp[640];
    snprintf(cp, sizeof cp, "%s/Chain", root);
    mkdir(cp, 0777);
    for (int i = 1; i <= 80; i++) {
        snprintf(cp, sizeof cp, "%s/Chain/T%d", root, i);
        FILE *cf = fopen(cp, "w");
        if (cf) {
            if (i < 80)
                fprintf(cf, "| tail %d\nObey HostFS::Test.$.Chain.T%d\n", i, i + 1);
            else
                fputs("Echo tail end\n", cf);
            fclose(cf);
        }
        snprintf(cp, sizeof cp, "%s/Chain/N%d", root, i);
        cf = fopen(cp, "w");
        if (cf) {
            if (i < 80)
                fprintf(cf, "Obey HostFS::Test.$.Chain.N%d\n| after %d\n", i + 1, i);
            else
                fputs("Echo deep\n", cf);
            fclose(cf);
        }
    }
    int tails = 0;
    for (int k = 0; k < 7; k++)
        tails += says("Obey HostFS::Test.$.Chain.T1", "tail end\n\r");
    check(tails == 7 && says("Obey HostFS::Test.$.Chain.N1", "deep\n\r"),
          "*Obey -- the last line is a tail call (80 files chained, 7 times), no level "
          "left behind; 80 deep where a line follows", "%d %s", tails, out);

    ros_vector_release_native(WRCHV, wrch, 0);
}

/* ---- commands that call themselves for ever (task.h, ros_stack_room) ---- */

/* One recursion run to its end on the calling thread: the error it ended
 * with, the levels it went (Test$Depth, which each level adds one to), and
 * the room each stack had before it and at the refusal, if one ended it */
struct recursion {
    const char *what, *cmd;
    uint32_t err;
    unsigned depth;
    size_t native0;
    uint32_t svc0, used0;
    int refused;
    struct ros_stack_refusal at;
    int echoed;                         /* a line after the recursive one ran */
    uint32_t rma_grew;
};

static void recurse(struct recursion *r)
{
    uint32_t used, free_bytes;
    cli("SetEval Test$Depth 0");
    ros_rma_stats(&r->used0, &free_bytes);
    unsigned before = ros_stack_last_refusal()->count;
    r->native0 = ros_native_stack_left();
    r->svc0 = ros_svc_sp - ROS_SVCSTACK_BASE;
    r->err = cli(r->cmd);
    /* a BASIC program's ON ERROR prints what ended it: "ERR=&<number>" */
    const char *said = strstr(out, "ERR=&");
    if (!r->err && said)
        r->err = (uint32_t)strtoul(said + 5, NULL, 16);
    r->echoed = strstr(out, "back") != NULL;
    r->refused = ros_stack_last_refusal()->count != before;
    r->at = *ros_stack_last_refusal();
    ros_rma_stats(&used, &free_bytes);
    r->rma_grew = used > r->used0 ? used - r->used0 : 0;
    cli("Echo <Test$Depth>");
    r->depth = (unsigned)strtoul(out, NULL, 10);
}

/* Each level's cost, from the room before and at the refusal.  Also the
 * line in the log that says it */
static void recursion_report(const struct recursion *r, const char *thread)
{
    unsigned n = r->depth ? r->depth : 1;
    if (r->refused && r->at.native != SIZE_MAX)
        ros_console_printf("        stack: %s, %s: &%X after %u levels, %zu bytes of native "
                           "stack and %u of SVC stack a level; %zu and %u left\n", r->what,
                           thread, r->err, r->depth, (r->native0 - r->at.native) / n,
                           (r->svc0 - r->at.svc) / n, r->at.native, r->at.svc);
    else
        ros_console_printf("        stack: %s, %s: &%X after %u levels\n", r->what, thread,
                           r->err, r->depth);
}

static int recursion_ends(const struct recursion *r, uint32_t err, unsigned least)
{
    return r->err == err && r->depth >= least && !r->echoed && r->rma_grew < 1024 &&
           (!r->refused || r->at.native >= ROS_STACK_ALIAS_NATIVE - ROS_STACK_LEVEL);
}

/* The cases are:
 *   - the heaviest cycle (a run action, its alias, *Obey, the line)
 *   - an application whose !Run runs it again on its last line (the tail
 *     call, so no file stays open)
 *   - an Obey file obeying itself on its last line
 *   - an alias expanding itself
 *   - a BASIC program starting itself as the application (OSCLI "BASICVFP
 *     -quit" of its own file, with no alias, no *Obey and no run action).
 *     Each start nests, and StartApplication asks for the room. */
enum { REC_RUN, REC_APP, REC_OBEY, REC_ALIAS, REC_BASIC, REC_N };
static struct recursion rec_main[REC_N], rec_task[REC_N];

static void recursion_cases(struct recursion *r)
{
    static const char *const what[REC_N][2] = {
        { "a run action running its file again", "HostFS::Rec.$.RecRun" },
        { "an application's !Run running it again", "HostFS::Rec.$.!RecApp" },
        { "an Obey file's last line obeying it", "Obey HostFS::Rec.$.RecObey" },
        { "an alias expanding itself", "RecLoop" },
        { "a BASIC program starting itself", "BASICVFP -quit HostFS::Rec.$.RecBas" },
    };
    cli("Set Alias$RecLoop SetEval Test$Depth Test$Depth+1|MRecLoop");
    for (unsigned k = 0; k < REC_N; k++) {
        r[k].what = what[k][0], r[k].cmd = what[k][1];
        recurse(&r[k]);
    }
    cli("Unset Alias$RecLoop");
}

/* A program's end (OS_Exit) on a task's thread ends the task, where the
 * default handler (environment.c, end_program) brings it back to the
 * command that entered it on the main thread.  The BASIC case's programs
 * end so there too, under an exit handler of the test's own. */
static void rec_exit_handler(struct ros_cpu *s)
{
    (void)s;
    ros_module_app_exit();                      /* back to the command that entered it */
    ros_env_end_program(0);                     /* none: the task ends */
}

static void recursion_task(void *arg)
{
    (void)arg;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ROS_ENV_EXIT, c.r[1] = ros_native_entry(rec_exit_handler, "selftest:RecExit");
    ros_swi(&c, XOS_ChangeEnvironment);
    recursion_cases(rec_task);
}

/* A BASIC program's FN recursing without end, in one program (#64): each
 * level is C frames of the translated interpreter, far more than BASIC's
 * own stack takes. The native stack's room is therefore asked at every
 * call, and its refusal is BASIC's "No room for function/procedure call"
 * (37). The program traps it, as on RISC OS. The box used to take a
 * SIGSEGV in basicvfp_FACTOR instead */
static struct recursion rec_fn_main, rec_fn_task;

static void recursion_fn_task(void *arg)
{
    (void)arg;
    struct ros_cpu c;
    ros_cpu_enter(&c);
    c.r[0] = ROS_ENV_EXIT, c.r[1] = ros_native_entry(rec_exit_handler, "selftest:RecFnExit");
    ros_swi(&c, XOS_ChangeEnvironment);
    recurse(&rec_fn_task);
}

static void put_in(const char *dir, const char *leaf, const char *data)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, leaf);
    FILE *f = fopen(p, "wb");
    if (f) {
        fputs(data, f);
        fclose(f);
    }
}

static void recursion(void)
{
    /* A disc of its own, "Rec", in memory.  In the box it is the
     * initramfs's, because the share's 9p takes 30 ms a level and the
     * levels go thousands deep.  Hosted, it is in TMPDIR */
    char rec[600], p[700];
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = "/tmp";
    mkdir(tmp, 0777);
    snprintf(rec, sizeof rec, "%s/rosgd-rec-XXXXXX", tmp);
    if (!mkdtemp(rec)) {
        check(0, "Recursion -- no directory for its disc", "no directory for its disc");
        return;
    }
    ros_hostfs_mount("Rec", rec);
    ros_vector_claim_native(WRCHV, wrch, 0);
    put_in(rec, "RecRun,feb", "SetEval Test$Depth Test$Depth+1\nHostFS::Rec.$.RecRun\n"
                              "Echo back\n");
    put_in(rec, "RecObey,feb", "SetEval Test$Depth Test$Depth+1\nObey HostFS::Rec.$.RecObey\n");
    snprintf(p, sizeof p, "%s/!RecApp", rec);
    mkdir(p, 0777);
    put_in(rec, "!RecApp/!Run,feb", "SetEval Test$Depth Test$Depth+1\n/<Obey$Dir>\n");
    put_in(rec, "RecBas,fd1", "10 ON ERROR PRINT \"ERR=&\";~ERR:END\n"
                              "20 OSCLI \"SetEval Test$Depth Test$Depth+1\"\n"
                              "30 OSCLI \"BASICVFP -quit HostFS::Rec.$.RecBas\"\n"
                              "40 END\n");
    put_in(rec, "RecFn,fd1", "10 ON ERROR OSCLI \"SetEval Test$Depth \"+STR$d%:"
                             "PRINT \"ERR=&\";~ERR:END\n"
                             "20 d%=0:PRINT FNr(1)\n"
                             "30 END\n"
                             "40 DEF FNr(n%):d%=n%:=FNr(n%+1)\n");

    /* On the main thread: 8 MB of native stack in the box (hosted, what
     * the shell gives it: make's 64 MB).  The run action's files stay
     * open, a level each, and FileSwitch's 255 handles run out first ("Too
     * many open files", &C0, before the stack does); the rest end at the
     * room they ask for.  Hosted under make, the Obey file's case ends at the SVC
     * stack's room, 40 bytes a level */
    recursion_cases(rec_main);
    const char *mt = "the main thread";
    for (unsigned k = 0; k < REC_N; k++)
        recursion_report(&rec_main[k], mt);
    int m1 = (rec_main[REC_RUN].err == 0xC0 || recursion_ends(&rec_main[REC_RUN], 0x414, 17)) &&
             rec_main[REC_RUN].depth >= 17 && !rec_main[REC_RUN].echoed &&
             rec_main[REC_RUN].rma_grew < 1024 &&
             recursion_ends(&rec_main[REC_APP], 0x414, 17) &&
             recursion_ends(&rec_main[REC_OBEY], 0x414, 17) &&
             recursion_ends(&rec_main[REC_ALIAS], 0x1E1, 17) &&
             recursion_ends(&rec_main[REC_BASIC], 0x414, 17);
    check(m1 && says("Echo still here", "still here\n\r"),
          "Recursion on the main thread -- a run action, an application's !Run, an Obey file's "
          "last line, a BASIC program starting itself: \"Not enough stack to call filing "
          "system\" (&414); an alias: \"Expansion too complex\"; each at the stacks' room, none "
          "left behind",
          "&%X %u, &%X %u, &%X %u, &%X %u, &%X %u", rec_main[0].err, rec_main[0].depth,
          rec_main[1].err, rec_main[1].depth, rec_main[2].err, rec_main[2].depth,
          rec_main[3].err, rec_main[3].depth, rec_main[4].err, rec_main[4].depth);

    /* On a Wimp task's thread: 8 MB of native stack, the main thread's,
     * where it was 1 MB. fix1's 64 alias levels overflowed that (the run
     * action, 16K a level), giving a SIGSEGV and the box gone, as an Obey
     * file obeying itself on its last line always did.  With the room the main thread
     * has, it ends as the main thread does: the run action's files stay
     * open, a level each, so FileSwitch's 255 handles may run out before
     * the stack's room does. */
    struct ros_task *t = ros_task_create(0, recursion_task, NULL);
    if (t) {
        ros_task_switch(t);                     /* it runs, ends, and the baton comes back */
        ros_task_destroy(t);
    }
    const char *tt = "a task's thread";
    for (unsigned k = 0; k < REC_N; k++)
        recursion_report(&rec_task[k], tt);
    int t1 = t && (rec_task[REC_RUN].err == 0xC0 ||
                   recursion_ends(&rec_task[REC_RUN], 0x414, 17)) &&
             rec_task[REC_RUN].depth >= 17 &&
             recursion_ends(&rec_task[REC_APP], 0x414, 17) &&
             recursion_ends(&rec_task[REC_OBEY], 0x414, 17) &&
             recursion_ends(&rec_task[REC_ALIAS], 0x1E1, 17) &&
             recursion_ends(&rec_task[REC_BASIC], 0x414, 17) &&
             (!rec_task[REC_RUN].refused || rec_task[REC_RUN].at.need == ROS_STACK_FILE) &&
             rec_task[REC_BASIC].refused && rec_task[REC_BASIC].at.need == ROS_STACK_FILE;
    check(t1 && says("Echo still here", "still here\n\r"),
          "Recursion on a task's thread (8 MB, the main thread's room) -- a run action, an "
          "application's !Run, an Obey file's last line, a BASIC program starting itself: &414, "
          "or &C0 where the handles go first; an alias: &1E1; each at a limit, the box still up",
          "&%X %u, &%X %u, &%X %u, &%X %u, &%X %u", rec_task[0].err, rec_task[0].depth,
          rec_task[1].err, rec_task[1].depth, rec_task[2].err, rec_task[2].depth,
          rec_task[3].err, rec_task[3].depth, rec_task[4].err, rec_task[4].depth);

    /* BASIC's FN recursion, on both threads (#64) */
    rec_fn_main.what = rec_fn_task.what = "a BASIC FN calling itself";
    rec_fn_main.cmd = rec_fn_task.cmd = "BASICVFP -quit HostFS::Rec.$.RecFn";
    recurse(&rec_fn_main);
    struct ros_task *ft = ros_task_create(0, recursion_fn_task, NULL);
    if (ft) {
        ros_task_switch(ft);
        ros_task_destroy(ft);
    }
    recursion_report(&rec_fn_main, mt);
    recursion_report(&rec_fn_task, tt);
    int f1 = ft && recursion_ends(&rec_fn_main, 0x25, 200) &&
             recursion_ends(&rec_fn_task, 0x25, 200) && rec_fn_main.refused &&
             rec_fn_main.at.need == ROS_STACK_BASIC && rec_fn_task.refused &&
             rec_fn_task.at.need == ROS_STACK_BASIC;
    check(f1 && says("Echo still here", "still here\n\r"),
          "Recursion of a BASIC FN, on the main thread and a task's -- BASIC's \"No room for "
          "function/procedure call\" (37), trapped by the program, at the native stack's room "
          "(#64)", "&%X %u, &%X %u", rec_fn_main.err, rec_fn_main.depth, rec_fn_task.err,
          rec_fn_task.depth);

    /* The margins hold what they say (task.h): the heaviest cycle is less
     * than a ROS_STACK_LEVEL a level, so a file's room is reached before an
     * alias's in any cycle through a file */
    const struct recursion *hr = rec_task[REC_RUN].refused ? &rec_task[REC_RUN]
                                                          : &rec_task[REC_APP];
    size_t level = hr->refused && hr->depth ? (hr->native0 - hr->at.native) / hr->depth : 0;
    check(level > 0 && level < ROS_STACK_LEVEL,
          "Recursion -- the heaviest cycle's native frames, a run action's, within "
          "ROS_STACK_LEVEL", "%zu bytes a level", level);

    /* An application started under alias expansions comes back to them
     * here (runtime/module.c), and they go on with their next lines,
     * though its own expansions have been round the kernel's ring and
     * taken their buffers (oscli.c: they kept copies as it started).
     * RISC OS 5.30 (bigmacfarm): a BASIC program run by its type from an
     * Obey file, its alias chain fifteen deep, comes back whole, "prog
     * end", and the Obey file's next line runs.  Here too, under the run
     * action's alias and BASIC's. That gave "Expansion too complex" as
     * the program ended, and no next line, before.  Under an alias of
     * two lines of its own, which RISC OS never comes back to (ROSGD's
     * rule), fifteen deep gives its second line.  Sixteen gives the
     * chain's &1E1 after the innermost line, which the program's ON ERROR
     * prints, and then its second line. */
    char chain[80];
    for (int k = 1; k <= 16; k++) {
        if (k < 16)
            snprintf(chain, sizeof chain, "Set Alias$RecC%d RecC%d", k, k + 1);
        else
            snprintf(chain, sizeof chain, "Set Alias$RecC16 Echo innermost");
        cli(chain);
    }
    for (int k = 15; k <= 16; k++) {
        snprintf(chain, sizeof chain, "RecP%d,fd1", k);
        char prog[200];
        snprintf(prog, sizeof prog, "10 ON ERROR PRINT \"ERR=&\";~ERR:END\n"
                                    "20 OSCLI \"RecC%d\"\n30 PRINT \"prog end\"\n", 17 - k);
        put_in(rec, chain, prog);
    }
    put_in(rec, "RecQ,feb", "HostFS::Rec.$.RecP15\nEcho after-P\n");
    cli("Set Alias$RecOuter HostFS::Rec.$.RecP%0|MEcho after-P");
    int x1 = says("Obey HostFS::Rec.$.RecQ", "innermost\n\rprog end\n\rafter-P\n\r");
    int x2 = says("RecOuter 15", "innermost\n\rprog end\n\rafter-P\n\r");
    int x3 = says("RecOuter 16", "innermost\n\rERR=&1E1\n\rafter-P\n\r");
    int x4 = says("RecC2", "innermost\n\r") && cli("RecC1") == 0x1E1 &&
             strcmp(out, "innermost\n\r") == 0;
    check(x1 && x2 && x3 && x4,
          "Aliases an application comes back to -- a BASIC program run by its type from an Obey "
          "file, its alias chain 15 deep: back whole, the next line run, as RISC OS 5.30; under "
          "an alias of two lines, 15 deep and 16 (&1E1 to its ON ERROR): the second line run; "
          "the ring whole after",
          "%d %d %d %d", x1, x2, x3, x4);
    for (int k = 1; k <= 16; k++) {
        snprintf(chain, sizeof chain, "Unset Alias$RecC%d", k);
        cli(chain);
    }
    cli("Unset Alias$RecOuter");

    ros_vector_release_native(WRCHV, wrch, 0);
    cli("Unset Test$Depth");
    ros_hostfs_unmount("Rec");
    remove_tree(rec);
}

/* ---- UpCall_ModifyingFile, as a client on UpCallV sees it ---- */

/* Each OS_UpCall 3 as one line: R9, then R1's path.  For a close or an
 * ensure (&103, &200) it is "#" and the handle instead.  A rename adds
 * R2.  R6 and R8 are checked as they come */
static char ups[2048];
static unsigned nups, ups_bad_r6r8;

static int on_upcall(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] != 3)
        return ROS_VECTOR_PASS;
    size_t n = strlen(ups);
    char item[600];
    if (s->r[9] == 0x103 || s->r[9] == 0x200)
        snprintf(item, sizeof item, "%X #%u", s->r[9], s->r[1]);
    else if (s->r[9] == 0x208)
        snprintf(item, sizeof item, "%X %s %s", s->r[9], (const char *)ros_ptr(s->r[1]),
                 (const char *)ros_ptr(s->r[2]));
    else
        snprintf(item, sizeof item, "%X %s", s->r[9], (const char *)ros_ptr(s->r[1]));
    snprintf(ups + n, sizeof ups - n, "%s%s", n ? "; " : "", item);
    /* R6 the special field (none), R8 HostFS's information word */
    if (s->r[6] != 0 || s->r[8] != 0x0498FFDCu)
        ups_bad_r6r8++;
    nups++;
    return ROS_VECTOR_PASS;
}

/* A client that opens a file while an open's UpCall is out (the race a
 * stream's handle must be taken before: FileSwitch's AllocateStream comes
 * before TryToOpenFile) */
static uint32_t race_inner;

static int on_upcall_race(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == 3 && s->r[9] == 0x102 && !race_inner &&
        strcmp((const char *)ros_ptr(s->r[1]), ":Test.$.Up2.RaceA") == 0) {
        struct ros_cpu c;
        ros_cpu_enter(&c);
        strcpy(ros_ptr(text + 768), "HostFS::Test.$.Up2.RaceB");
        c.r[0] = 0x40, c.r[1] = text + 768;
        ros_swi(&c, XOS_Find);
        race_inner = c.v ? 0xFFFFFFFFu : c.r[0];
    }
    return ROS_VECTOR_PASS;
}

/* ResourceFS's UpCalls as it registers and deregisters files: R9, R1's
 * string, R8 */
static char rfs_ups[256];

static int on_upcall_rfs(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    if (s->r[0] == 3 && s->r[8] == 0x0001002Eu) {
        size_t n = strlen(rfs_ups);
        snprintf(rfs_ups + n, sizeof rfs_ups - n, "%s%X \"%s\" R6 %u", n ? "; " : "", s->r[9],
                 (const char *)ros_ptr(s->r[1]), s->r[6]);
    }
    return ROS_VECTOR_PASS;
}

static int upcalls_are(const char *want)
{
    int ok = strcmp(ups, want) == 0;
    if (!ok)
        ros_console_printf("        UpCalls \"%s\", want \"%s\"\n", ups, want);
    ups[0] = 0;
    return ok;
}

static void upcalls(void)
{
    uint32_t data = text + 1024;
    char want[256];

    /* OS_FSControl 13's block: the information word where FileSwitch's
     * control block has it (fscb_info, +32), which the Filer reads for a
     * viewer's filing system (s/CacheDir).  HostFS's is the team's
     * HostFS's (FSInfoWord), and ResourceFS's is its fsnumber_resourcefs
     * :OR: fsinfo_readonly */
    strcpy(ros_ptr(data), "HostFS");
    uint32_t lh[8] = { 13, data, 0 };
    int vh = swi(XOS_FSControl, lh);
    uint32_t lr[8] = { 13, 46, 0 };
    int vr = swi(XOS_FSControl, lr);
    uint32_t wh = !vh && lh[2] ? ros_ld32(lh[2] + 32) : 0, wr = !vr && lr[2] ? ros_ld32(lr[2] + 32) : 0;
    check(wh == 0x0498FFDCu && wr == 0x0001002Eu && ros_ld32(lh[2]) == 220,
          "FileSwitch -- OS_FSControl 13's block: the information word at +32 (fscb_info), "
          "HostFS &0498FFDC, ResourceFS &0001002E, as the Filer reads it",
          "HostFS &%X, ResourceFS &%X", wh, wr);

    ups[0] = 0, nups = 0, ups_bad_r6r8 = 0;
    ros_vector_claim_native(ROS_UPCALLV, on_upcall, 0);

    /* OS_File: create directory, once.  One already there is left alone,
     * untold (int_DeleteCreateFileOp) */
    uint32_t d1[8] = { 8, name("HostFS::Test.$.Up") };
    swi(XOS_File, d1);
    uint32_t d2[8] = { 8, name("HostFS::Test.$.Up") };
    swi(XOS_File, d2);
    int u1 = upcalls_are("8 :Test.$.Up");

    /* A save on HostFS, which opts out of saves (fsinfo_dontusesave): a
     * create and an open for update, the stream closed unmodified
     * (int_DoSaveFile).  Then a create (OS_File 11) */
    strcpy(ros_ptr(data), "hello");
    uint32_t sv[8] = { 10, name("HostFS::Test.$.Up.F"), 0xFFF, 0, data, data + 5 };
    swi(XOS_File, sv);
    uint32_t cr[8] = { 11, name("HostFS::Test.$.Up.C"), 0xFFD, 0, 0, 10 };
    swi(XOS_File, cr);
    int u2 = upcalls_are("7 :Test.$.Up.F; 102 :Test.$.Up.F; 7 :Test.$.Up.C");

    /* Catalogue information, stamp, set type: each WriteInfo (1), as
     * FileSwitch passes them all (WriteInfoFilePresent, StampSetTypeFileOp) */
    uint32_t wa[8] = { 4, name("HostFS::Test.$.Up.F"), 0, 0, 0, 0x13 };
    swi(XOS_File, wa);
    uint32_t st[8] = { 9, name("HostFS::Test.$.Up.F") };
    swi(XOS_File, st);
    uint32_t ty[8] = { 18, name("HostFS::Test.$.Up.F"), 0xFFD };
    swi(XOS_File, ty);
    int u3 = upcalls_are("1 :Test.$.Up.F; 1 :Test.$.Up.F; 1 :Test.$.Up.F");

    /* Rename: &208, both names */
    uint32_t rn[8] = { 25, name("HostFS::Test.$.Up.F"), name2("HostFS::Test.$.Up.G") };
    swi(XOS_FSControl, rn);
    int u4 = upcalls_are("208 :Test.$.Up.F :Test.$.Up.G");

    /* Streams: OpenUp and OpenOut open for update (&102), OpenOut creating
     * what is not there first (TryToOpenFile).  A close tells of a modified
     * stream only (&103, its handle), and OpenOut's is modified.  Then an
     * ensure (&200) */
    uint32_t o1[8] = { 0xC0, name("HostFS::Test.$.Up.G") };
    swi(XOS_Find, o1);
    uint32_t c1[8] = { 0, o1[0] };
    swi(XOS_Find, c1);
    uint32_t o2[8] = { 0xC0, name("HostFS::Test.$.Up.G") };
    swi(XOS_Find, o2);
    uint32_t b2[8] = { 'x', o2[0] };
    swi(XOS_BPut, b2);
    uint32_t e2[8] = { 6, o2[0], 100 };
    swi(XOS_Args, e2);
    uint32_t c2[8] = { 0, o2[0] };
    swi(XOS_Find, c2);
    uint32_t o3[8] = { 0x80, name("HostFS::Test.$.Up.H") };
    swi(XOS_Find, o3);
    uint32_t c3[8] = { 0, o3[0] };
    swi(XOS_Find, c3);
    snprintf(want, sizeof want,
             "102 :Test.$.Up.G; 102 :Test.$.Up.G; 200 #%u; 103 #%u; 7 :Test.$.Up.H; "
             "102 :Test.$.Up.H; 103 #%u", o2[0], o2[0], o3[0]);
    int u5 = upcalls_are(want);

    /* Delete: once.  With nothing there, nothing is done or told.
     * *Access: WriteInfo (1) for a filing system without its own access string */
    uint32_t de[8] = { 6, name("HostFS::Test.$.Up.G") };
    swi(XOS_File, de);
    uint32_t de2[8] = { 6, name("HostFS::Test.$.Up.G") };
    swi(XOS_File, de2);
    cli("Access HostFS::Test.$.Up.H WR/r");
    int u6 = upcalls_are("6 :Test.$.Up.G; 1 :Test.$.Up.H");

    /* *Wipe through FileSwitch's own calls: each object deleted, told */
    cli("Wipe HostFS::Test.$.Up ~CFR~V");
    int u7 = upcalls_are("6 :Test.$.Up.C; 6 :Test.$.Up.H; 6 :Test.$.Up");

    /* OpenOut of a file that is there: its load and exec addresses and
     * attributes kept, its extent 0 (s/OSFind: "OpenOut preserves file
     * load/exec/attr"), and only the open for update told.  OS_Args 3 on an
     * OpenUp stream does not make it modified, because FileSwitch sets that
     * on writes.  So its close is not told */
    uint32_t cd[8] = { 8, name("HostFS::Test.$.Up2") };
    swi(XOS_File, cd);
    uint32_t sb[8] = { 10, name("HostFS::Test.$.Up2.Prog"), 0xFFB, 0, data, data + 5 };
    swi(XOS_File, sb);
    uint32_t ac[8] = { 4, name("HostFS::Test.$.Up2.Prog"), 0, 0, 0, 0x33 };
    swi(XOS_File, ac);
    uint32_t r0[8] = { 17, name("HostFS::Test.$.Up2.Prog") };
    swi(XOS_File, r0);
    ups[0] = 0;
    uint32_t oo[8] = { 0x80, name("HostFS::Test.$.Up2.Prog") };
    int voo = swi(XOS_Find, oo);
    uint32_t ext[8] = { 2, oo[0] };
    swi(XOS_Args, ext);
    uint32_t oc[8] = { 0, oo[0] };
    swi(XOS_Find, oc);
    uint32_t r1[8] = { 17, name("HostFS::Test.$.Up2.Prog") };
    swi(XOS_File, r1);
    uint32_t ou[8] = { 0xC0, name("HostFS::Test.$.Up2.Prog") };
    swi(XOS_Find, ou);
    uint32_t se[8] = { 3, ou[0], 7 };
    swi(XOS_Args, se);
    uint32_t sf[8] = { 0xFE, ou[0] };
    swi(XOS_Args, sf);
    uint32_t uc[8] = { 0, ou[0] };
    swi(XOS_Find, uc);
    snprintf(want, sizeof want, "102 :Test.$.Up2.Prog; 103 #%u; 102 :Test.$.Up2.Prog", oo[0]);
    /* (the close restamps the date because the stream was modified, and the type is kept) */
    int kept = !voo && (r1[2] & 0xFFF00000u) == 0xFFF00000u && (r1[2] >> 8 & 0xFFF) == 0xFFB &&
               (r0[2] >> 8 & 0xFFF) == 0xFFB && r1[5] == 0x33 && r1[4] == 0 && ext[2] == 0 &&
               !(sf[0] & 0x100);
    int u8 = upcalls_are(want);

    /* An open whose UpCall another open interrupts: two handles, each its
     * own file */
    strcpy(ros_ptr(data), "AAAA");
    uint32_t fa[8] = { 10, name("HostFS::Test.$.Up2.RaceA"), 0xFFD, 0, data, data + 4 };
    swi(XOS_File, fa);
    strcpy(ros_ptr(data), "BBBBBBBB");
    uint32_t fb[8] = { 10, name("HostFS::Test.$.Up2.RaceB"), 0xFFD, 0, data, data + 8 };
    swi(XOS_File, fb);
    race_inner = 0;
    ros_vector_claim_native(ROS_UPCALLV, on_upcall_race, 0);
    uint32_t ro[8] = { 0xC0, name("HostFS::Test.$.Up2.RaceA") };
    int vro = swi(XOS_Find, ro);
    ros_vector_release_native(ROS_UPCALLV, on_upcall_race, 0);
    uint32_t ea[8] = { 2, ro[0] }, eb[8] = { 2, race_inner };
    int vea = swi(XOS_Args, ea), veb = swi(XOS_Args, eb);
    uint32_t ca[8] = { 0, ro[0] }, cb[8] = { 0, race_inner };
    int vca = swi(XOS_Find, ca), vcb = swi(XOS_Find, cb);
    uint32_t wipe[8] = { 27, name("HostFS::Test.$.Up2"), 0, 0x3 };
    int vwipe = swi(XOS_FSControl, wipe);
    int raced = !vro && race_inner && race_inner != 0xFFFFFFFFu && race_inner != ro[0] &&
                !vea && !veb && ea[2] == 4 && eb[2] == 8 && !vca && !vcb && !vwipe &&
                !host_exists("Up2");
    ups[0] = 0;

    ros_vector_release_native(ROS_UPCALLV, on_upcall, 0);

    /* ResourceFS: files registered and deregistered, each an UpCall for ""
     * (every directory), Create then Delete, its information word
     * (s/ResourceFS, modifyingfiles) */
    uint32_t blk = text + 1536;
    memset(ros_ptr(blk), 0, 64);
    ros_st32(blk + 0, 20 + 12 + 4 + 4);                 /* the entry, then the end */
    ros_st32(blk + 4, 0xFFFFFF00u), ros_st32(blk + 8, 0), ros_st32(blk + 12, 3);
    ros_st32(blk + 16, 0x11);
    strcpy(ros_ptr(blk + 20), "RfsUpTest.F");           /* 12 bytes with its 0 */
    ros_st32(blk + 32, 3 + 4);
    memcpy(ros_ptr(blk + 36), "abc", 3);
    rfs_ups[0] = 0;
    ros_vector_claim_native(ROS_UPCALLV, on_upcall_rfs, 0);
    uint32_t rg[8] = { blk };
    int vrg = swi(XResourceFS_RegisterFiles, rg);
    uint32_t rt[8] = { 17, name("Resources:$.RfsUpTest.F") };
    swi(XOS_File, rt);
    uint32_t dr[8] = { blk };
    int vdr = swi(XResourceFS_DeregisterFiles, dr);
    ros_vector_release_native(ROS_UPCALLV, on_upcall_rfs, 0);
    int rfs_told = !vrg && !vdr && rt[0] == 1 && rt[4] == 3 &&
                   strcmp(rfs_ups, "7 \"\" R6 0; 6 \"\" R6 0") == 0;

    check(rfs_told,
          "ResourceFS -- UpCall_ModifyingFile as files are registered and deregistered: \"\", "
          "Create then Delete, its information word &0001002E",
          "registered %d, found %u (%u bytes), deregistered %d; UpCalls %s", !vrg, rt[0], rt[4],
          !vdr, rfs_ups);
    check(kept && u8,
          "FileSwitch -- OpenOut of a file that is there keeps its type and attributes, its "
          "extent 0, told only as an open for update; OS_Args 3 leaves a stream unmodified, "
          "its close untold",
          "type &%03X -> &%03X, attributes &%X, length %u, extent %u, status &%X; UpCalls %s",
          r0[2] >> 8 & 0xFFF, r1[2] >> 8 & 0xFFF, r1[5], r1[4], ext[2], sf[0],
          u8 ? "as expected" : "wrong");
    check(raced,
          "FileSwitch -- an open's handle is taken before its UpCall: a client opening a file "
          "during it gets another, each stream its own file, both closed",
          "outer %u (%d), inner %u; extents %u %u (%d %d); closed %d %d; wiped %d",
          ro[0], vro, race_inner, ea[2], eb[2], vea, veb, vca, vcb, vwipe);
    check(u1 && u2 && u3 && u4 && u5 && u6 && u7 && !ups_bad_r6r8,
          "FileSwitch -- UpCall_ModifyingFile (OS_UpCall 3) before each change, as "
          "DoUpCallModifyingFile: R9 the upfs code, R1 the path past \"HostFS:\", R6 0, R8 "
          "the information word -- create directory, save, create, write info, stamp, set "
          "type, rename, open for update, ensure, close, delete, *Access, *Wipe",
          "%u %u %u %u %u %u %u, %u of %u with R6/R8 wrong", u1, u2, u3, u4, u5, u6, u7,
          ups_bad_r6r8, nups);
}

/* ---- the Free module's entry ------------------------------------------------- */

/* The Free module's windows (*ShowFree, the Free entry on HostFSFiler's
 * menu) read a filing system's space through the entry the filing
 * system gave Free_Register (Desktop/Free/Doc/FreeSpace).  Without one Free
 * says "Unknown filing system".  Free keeps them in a list in its
 * workspace (Desktop/Free/s/Free): fs_list at +16, each node fs_next,
 * fs_prev, fs_number, fs_entry, fs_r12.  How many nodes filing system fs
 * has, and the last one's entry and R12. */
static unsigned free_nodes(uint32_t fs, uint32_t *entry, uint32_t *r12)
{
    struct ros_module *m = ros_module_first();
    while (m && !(m->title && strcmp(m->title, "Free") == 0))
        m = m->next;
    if (!m || !m->private_word || !ros_ld32(m->private_word))
        return 0;
    unsigned n = 0;
    for (uint32_t node = ros_ld32(ros_ld32(m->private_word) + 16); node; node = ros_ld32(node))
        if (ros_ld32(node + 8) == fs) {
            n++;
            *entry = ros_ld32(node + 12);
            *r12 = ros_ld32(node + 16);
        }
    return n;
}

/* The entry called as Free's CallEntry calls it (Desktop/Free/s/StartLoop):
 * R12 as registered, the return address pushed on the caller's stack
 * (Push "PC", then LDR PC), and LR is the list node.  LR is not a return
 * address.  Gives 1 if it came back there, pulling it, with every
 * register but R0 kept.  R0-R7
 * are back in r.  Z goes in as *z and comes back there, V in v. */
static int free_call(uint32_t entry, uint32_t r12, uint32_t r[8], uint32_t *z, uint32_t *v)
{
    struct ros_cpu s, was;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    for (int i = 8; i < 12; i++)
        s.r[i] = 0x5EED0000u + (uint32_t)i;
    s.r[12] = r12;
    s.r[14] = 0x5EED000Eu;
    s.z = *z;
    s.r[13] -= 4;
    ros_st32(s.r[13], ROS_RETURN_TO_NATIVE);
    was = s;
    ros_call(&s, entry);
    int kept = s.r[15] == ROS_RETURN_TO_NATIVE && s.r[13] == was.r[13] + 4;
    for (int i = 1; i < 15; i++)
        if (i != 13 && s.r[i] != was.r[i])
            kept = 0;
    memcpy(r, s.r, 8 * sizeof r[0]);
    *z = s.z, *v = s.v;
    return kept;
}

static void free_entry(void)
{
    uint32_t entry = 0, r12 = 0, z = 0, v;
    unsigned n = free_nodes(220, &entry, &r12);
    check(n == 1, "HostFS -- registered with Free (Free_Register) once, as the ROM's modules "
                  "finished starting, so *ShowFree -FS HostFS opens a window",
          "%u entries for HostFS in Free's list", n);
    if (n != 1) {                       /* nothing to call: each of the entry's cases fails */
        for (int i = 0; i < 3; i++)
            check(0, "HostFS's Free entry", "not in Free's list");
        return;
    }
    uint32_t buf = text + 1024, dev = text + 1536;     /* names at +0 and +512 */

    /* 1: the device's name, the disc as it was mounted, R0 its length */
    strcpy(ros_ptr(dev), "test");
    memset(ros_ptr(buf), 0xFF, 32);
    uint32_t r1[8] = { 1, 220, buf, dev, 4, 5, 6, 7 };
    int k1 = free_call(entry, r12, r1, &z, &v);
    int ok1 = k1 && !v && r1[0] == 5 && strcmp(ros_ptr(buf), "Test") == 0;
    strcpy(ros_ptr(dev), "NoSuchDisc");
    uint32_t r1e[8] = { 1, 220, buf, dev };
    int k1e = free_call(entry, r12, r1e, &z, &v);
    int ok1e = k1e && v && errnum(r1e) == 0x1DCD3 &&
               strcmp(errmess(r1e), "Disc 'NoSuchDisc' not found") == 0;
    /* none gives the boot disc.  "HostFS" (no disc of that name) gives the
     * boot disc too, as the team's HostFS takes any device.  This is the
     * case of its filer's *ShowFree -FS HostFS HostFS */
    char boot[64] = "", hostfs[64] = "";
    ros_st8(dev, 0);
    uint32_t rb[8] = { 1, 220, buf, dev };
    int kb = free_call(entry, r12, rb, &z, &v) && !v;
    snprintf(boot, sizeof boot, "%s", kb ? (char *)ros_ptr(buf) : "");
    strcpy(ros_ptr(dev), "hostfs");
    uint32_t rh[8] = { 1, 220, buf, dev };
    int kh = free_call(entry, r12, rh, &z, &v) && !v;
    snprintf(hostfs, sizeof hostfs, "%s", kh ? (char *)ros_ptr(buf) : errmess(rh));
    int okh = kb && kh && boot[0] && strcmp(boot, hostfs) == 0;
    check(ok1 && ok1e && okh,
          "HostFS's Free entry, 1 -- the device's name: the disc's, as mounted, R0 its length "
          "with the terminator; a disc not there, &1DCD3; none or \"HostFS\", the boot disc; "
          "back by the address Free stacked",
          "%d %d: R0 %u \"%.20s\"; %d %d; boot \"%s\", HostFS \"%s\"", k1, ok1, r1[0],
          (char *)ros_ptr(buf), k1e, ok1e, boot, hostfs);

    /* 2 and 4: size, free and used, 32 and 64 bits, as OS_FSControl 49 and
     * 55 give them */
    strcpy(ros_ptr(dev), "Test");
    uint32_t fs49[8] = { 49, name("HostFS::Test.$") };
    swi(XOS_FSControl, fs49);
    uint32_t fs55[8] = { 55, name("HostFS::Test.$") };
    swi(XOS_FSControl, fs55);
    uint64_t size55 = fs55[3] | (uint64_t)fs55[4] << 32;
    memset(ros_ptr(buf), 0xFF, 32);
    uint32_t r2[8] = { 2, 220, buf, dev };
    int k2 = free_call(entry, r12, r2, &z, &v);
    uint32_t sz = ros_ld32(buf), fr = ros_ld32(buf + 4), us = ros_ld32(buf + 8);
    int ok2 = k2 && !v && r2[0] == 2 && sz == fs49[2] && fr <= sz && us == sz - fr;
    memset(ros_ptr(buf), 0xFF, 32);
    uint32_t r4[8] = { 4, 220, buf, dev };
    int k4 = free_call(entry, r12, r4, &z, &v);
    uint64_t sz64 = ros_ld32(buf) | (uint64_t)ros_ld32(buf + 4) << 32;
    uint64_t fr64 = ros_ld32(buf + 8) | (uint64_t)ros_ld32(buf + 12) << 32;
    uint64_t us64 = ros_ld32(buf + 16) | (uint64_t)ros_ld32(buf + 20) << 32;
    int ok4 = k4 && !v && r4[0] == 0 && sz64 == size55 && sz64 != 0 && fr64 <= sz64 &&
              us64 == sz64 - fr64;
    check(ok2 && ok4,
          "HostFS's Free entry, 2 and 4 -- size, free and used: 32 bits as OS_FSControl 49 "
          "gives them, 64 as 55 does, R0 0 for 4",
          "%d %d: %u %u %u vs %u; %d %d: R0 %u %llu %llu %llu vs %llu", k2, ok2, sz, fr, us,
          fs49[2], k4, ok4, r4[0], (unsigned long long)sz64, (unsigned long long)fr64,
          (unsigned long long)us64, (unsigned long long)size55);

    /* 3: Z set if the file is on the device.  A name on another disc is
     * not, and one with no disc may be.  0: nothing */
    uint32_t zs[3], ks = 1;
    const char *files3[3] = { ":Test.$.Hello", ":Other.$.Hello", "Hello" };
    for (int i = 0; i < 3; i++) {
        uint32_t r3[8] = { 3, 220, name2(files3[i]), dev };
        zs[i] = i != 1 ? 0 : 1;             /* the other way from the answer */
        ks &= (uint32_t)free_call(entry, r12, r3, &zs[i], &v) && !v && r3[0] == 3;
    }
    uint32_t r0[8] = { 0, 220, buf, dev }, z0 = 1;
    int k0 = free_call(entry, r12, r0, &z0, &v) && !v && r0[0] == 0 && z0 == 1;
    check(ks && zs[0] && !zs[1] && zs[2] && k0,
          "HostFS's Free entry, 3 -- Z set for a file on the device, clear for another disc's, "
          "set for a name with no disc; 0 -- nothing, every register kept",
          "%u: Z %u %u %u; %d", ks, zs[0], zs[1], zs[2], k0);
}

/* Wildcard matching: the usual cases, and a pattern with many stars against
 * a name that nearly matches, which must not take exponential time. */
static void wildcards(void)
{
    char name[128];
    memset(name, 'a', 100);
    name[100] = 0;
    int slow = fsw_wild_match("*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b", name);
    check(slow == 0, "FileSwitch -- a pattern of many stars against a long near miss is refused quickly",
          "many stars: %d", slow);
    check(fsw_wild_match("H*o", "Hello") && fsw_wild_match("He#lo", "hello") &&
              fsw_wild_match("a*b", "ab") && fsw_wild_match("*a", "ba") && fsw_wild_match("*", "") &&
              fsw_wild_match("a**b", "axxb") && fsw_wild_match("*.c", "x.c") &&
              !fsw_wild_match("a*#", "a") && !fsw_wild_match("#", "") && !fsw_wild_match("*a*", "xbx") &&
              !fsw_wild_match("ab", "abc") && !fsw_wild_match("abc", "ab"),
          "FileSwitch -- wildcards: * any run, # one character, no case",
          "wildcards: * any run, # one character");
}

/* A symbolic link in the share must not lead out of the disc, whether its
 * target exists or not.  A save onto a link whose target does not exist would
 * otherwise create the target, which lies outside. */
static void symlinks(void)
{
    uint32_t data = text + 1024;
    char out[600], target[700], linkpath[700], secret[700];
    snprintf(out, sizeof out, "%s-outside", root);
    mkdir(out, 0777);
    snprintf(target, sizeof target, "%s/new", out);
    snprintf(secret, sizeof secret, "%s/secret", out);
    snprintf(linkpath, sizeof linkpath, "%s/Dangling", root);
    FILE *f = fopen(secret, "w");
    if (f)
        fputs("secret", f), fclose(f);
    int made = symlink(target, linkpath) == 0;
    snprintf(linkpath, sizeof linkpath, "%s/Direct", root);
    made = made && symlink(secret, linkpath) == 0;
    check(made, "HostFS -- test links made", "test links made");

    strcpy(ros_ptr(data), "payload");
    uint32_t sv[8] = { 10, name("HostFS::Test.$.Dangling"), 0xFFF, 0, data, data + 7 };
    swi(XOS_File, sv);
    struct stat st;
    check(stat(target, &st) != 0,
          "HostFS -- a save onto a link to a missing target does not create the target outside the disc",
          "a save through a dangling link created %s", target);
    uint32_t op[8] = { 0x40, name("HostFS::Test.$.Direct") };
    int v = swi(XOS_Find, op);
    check(v == 1 || op[0] == 0, "HostFS -- a link to a file outside the disc is not opened",
          "a link to a file outside the disc was opened (handle %u)", op[0]);
    if (v == 0 && op[0]) {
        uint32_t cl[8] = { 0, op[0] };
        swi(XOS_Find, cl);
    }
    unlink(target);
    unlink(secret);
    rmdir(out);
}

void ros_selftest_files(void)
{
    text = ros_addr(ros_rma_alloc(2048));
    /* In the box, on the host share, as HostFS is used; hosted, in TMPDIR */
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    snprintf(root, sizeof root, "%s/rosgd-files-XXXXXX", tmp);
    mkdir(tmp, 0777);
    if (!mkdtemp(root)) {
        check(0, "FileSwitch -- no directory for the test disc", "no directory for the test disc");
        return;
    }
    ros_hostfs_mount("Test", root);
    uint32_t r[8] = { 14, 220 };
    check(swi(XOS_FSControl, r) == 0, "FileSwitch -- select HostFS by number", "select HostFS by number");
    uint32_t n[8] = { 33, 220, text + 1024, 32 };
    swi(XOS_FSControl, n);
    check(!strcmp(ros_ptr(text + 1024), "HostFS"), "FileSwitch -- FS 220 is HostFS", "FS 220 is %s",
          (char *)ros_ptr(text + 1024));

    streams();
    files();
    utilities();
    recursion();
    upcalls();
    free_entry();
    symlinks();
    wildcards();
    messages();
    resources();

    uint32_t cl[8] = { 0, 0 };
    swi(XOS_Find, cl);
    ros_hostfs_unmount("Test");
    remove_tree(root);
}
