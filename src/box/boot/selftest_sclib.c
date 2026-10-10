/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_sclib.c: SharedCLibrary, the module (modules/sharedclib,
 * package R5), hosted and in the box.
 *
 * Its surface is RISC OS 5.30's. tests/capps/sclib/sclib.bas is run here by
 * *BASIC -quit with its output redirected to a file, as the farm ran it.
 * It must print what 5.30 printed (tests/capps/farm/sclib.txt, made by
 * tests/capps/sclib/farm.py) line for line. That covers *Help, RMEnsure, the
 * module header, the SWI names both ways, the errors of the refusing SWIs,
 * the CLib messages C01-C76 through SharedCLibrary$Path, the two path
 * variables, and the RISC_OSLib messages. The one difference is the help
 * string's " ROSGD native", which every native module has.
 *
 * Registration itself, in the box, is the stub-only client's
 * (boot/selftest_capps.c, against 5.30's record of the same client). Here,
 * from native code, the checks cover the parts of it that no RISC OS client
 * of today reaches. A client's own statics are copied and zeroed (R5 >= R4)
 * and the library's template is put at the copy. A chunk that is not whole
 * slots is refused (C02), and so are stub descriptors where no memory is
 * (&411). Hosted, where there is no image, registration is refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fileswitch.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/capp.h"
#include "rosgd/clibimage.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "sclib_probe.h"
#include "selftest.h"
#include "sharedclib.h"

#define check ros_check

static int swi(uint32_t n, uint32_t r[10])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 10 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 10 * sizeof r[0]);
    return s.v;
}

static const os_error *err(const uint32_t r[10])
{
    return ros_ptr(r[0]);
}

/* The next line of [*p, end), NUL-terminated in line; 0 at the end */
static int next_line(const char **p, const char *end, char *line, size_t room)
{
    if (*p >= end)
        return 0;
    const char *nl = memchr(*p, '\n', (size_t)(end - *p));
    size_t n = (size_t)((nl ? nl : end) - *p);
    if (n >= room)
        n = room - 1;
    memcpy(line, *p, n);
    line[n] = 0;
    *p = nl ? nl + 1 : end;
    return 1;
}

/* A command through OS_CLI; its error, or NULL */
static const os_error *cli(const char *line)
{
    char *b = ros_rma_alloc((uint32_t)strlen(line) + 2);
    if (!b)
        return NULL;
    strcpy(b, line);
    uint32_t r[10] = { ros_addr(b) };
    int v = swi(XOS_CLI, r);
    ros_rma_free(b);
    return v ? err(r) : NULL;
}

/* A variable's value (expanded as a string), or "" if it is not there */
static void var(const char *name, char *out, size_t room)
{
    char *b = ros_rma_alloc(300);
    out[0] = 0;
    if (!b)
        return;
    strcpy(b, name);
    uint32_t r[10] = { ros_addr(b), ros_addr(b) + 64, 200, 0, 3 };
    if (!swi(XOS_ReadVarVal, r)) {
        size_t n = r[2] < room ? r[2] : room - 1;
        memcpy(out, b + 64, n);
        out[n] = 0;
    }
    ros_rma_free(b);
}

/* The two path variables the module sets at initialisation: only when a
 * variable is not there (setresourcevar looks at OS_ReadVarVal's R2 alone),
 * so a user's setting outlives *RMReInit, and a variable that has gone
 * comes back with its default */
static void paths(void)
{
    char a[64], b[64], c[64], d[64];
    const os_error *e = cli("Set SharedCLibrary$Path Mine:");
    if (!e)
        e = cli("Set RISC_OSLibrary$Path Yours:");
    if (!e)
        e = cli("RMReInit SharedCLibrary");
    var("SharedCLibrary$Path", a, sizeof a);
    var("RISC_OSLibrary$Path", b, sizeof b);
    const os_error *e2 = cli("Unset SharedCLibrary$Path");
    if (!e2)
        e2 = cli("Unset RISC_OSLibrary$Path");
    if (!e2)
        e2 = cli("RMReInit SharedCLibrary");
    var("SharedCLibrary$Path", c, sizeof c);
    var("RISC_OSLibrary$Path", d, sizeof d);
    check(!e && !e2 && !strcmp(a, "Mine:") && !strcmp(b, "Yours:") &&
              !strcmp(c, "Resources:$.Resources.CLib.") && !strcmp(d, "Resources:$.Resources.RISC_OSLib."),
          "SharedCLibrary: *RMReInit keeps SharedCLibrary$Path and RISC_OSLibrary$Path when they are set, "
          "and sets them when they are not (s/initmodule's setresourcevar)",
          "%s%s set: [%s] [%s]; unset: [%s] [%s]", e ? e->errmess : "", e2 ? e2->errmess : "", a, b, c, d);
}

/* ---- the surface, against 5.30's ---------------------------------------------------- */

static void surface(void)
{
    struct stat hs;
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    char root[512], p[600];
    snprintf(root, sizeof root, "%s/rosgd-sclib-XXXXXX", tmp);
    if (!mkdtemp(root)) {
        check(0, "SharedCLibrary: no directory for the test's disc", NULL);
        return;
    }
    ros_hostfs_mount("SCLTest", root);
    snprintf(p, sizeof p, "%s/sclib,fff", root);
    FILE *f = fopen(p, "wb");
    if (f) {
        fwrite(sclib_bas, 1, SCLIB_BAS_SIZE, f);
        fclose(f);
    }
    snprintf(p, sizeof p, "%s/Run,feb", root);
    f = fopen(p, "w");
    if (f) {
        fputs("WimpSlot 16M\n"
              "BASIC -quit HostFS::SCLTest.$.sclib { > HostFS::SCLTest.$.out }\n", f);
        fclose(f);
    }
    char *line = ros_rma_alloc(64);
    strcpy(line, "HostFS::SCLTest.$.Run");
    uint32_t r[10] = { ros_addr(line) };
    int v = swi(XOS_CLI, r);
    ros_rma_free(line);

    /* the redirection's file, whatever type HostFS gave it */
    static char got[8192];
    size_t n = 0;
    snprintf(p, sizeof p, "%s/out,ffd", root);
    f = fopen(p, "rb");
    if (!f) {
        snprintf(p, sizeof p, "%s/out", root);
        f = fopen(p, "rb");
    }
    if (f) {
        size_t k = fread(got, 1, sizeof got - 1, f);
        fclose(f);
        for (size_t i = 0; i < k; i++)          /* LF CR made LF, other CRs gone, as the record */
            if (got[i] != '\r')
                got[n++] = got[i];
    }
    got[n] = 0;

    const char *w = (const char *)sclib_farm, *wend = w + SCLIB_FARM_SIZE, *g = got, *gend = got + n;
    char want[300], mine[300], diff[700] = "";
    unsigned lines = 0, native = 0;
    for (;;) {
        int hw, hg = next_line(&g, gend, mine, sizeof mine);
        while ((hw = next_line(&w, wend, want, sizeof want)) && !strncmp(want, "# ", 2))
            ;
        if (!hw && !hg)
            break;
        /* the house's help string: 5.30's, then " ROSGD native" */
        char *at = hw ? strstr(want, "6.23 (15 May 2024)") : NULL;
        if (at && hg && strcmp(want, mine)) {
            char both[300];
            snprintf(both, sizeof both, "%.*s6.23 (15 May 2024) ROSGD native%s", (int)(at - want), want,
                     at + strlen("6.23 (15 May 2024)"));
            if (!strcmp(both, mine)) {
                native++;
                lines++;
                continue;
            }
        }
        if (!hw || !hg || strcmp(want, mine)) {
            snprintf(diff, sizeof diff, "line %u: 5.30 [%s], ROSGD [%s]", lines + 1, hw ? want : "(end)",
                     hg ? mine : "(end)");
            break;
        }
        lines++;
    }
    check(!v && !diff[0] && lines == 102 && native == 2,
          "SharedCLibrary: its surface as RISC OS 5.30's (sclib.bas against tests/capps/farm/sclib.txt) -- "
          "*Help and RMEnsure (6.23), its header and SWI names, APCS-A/R and the old module call refused "
          "(&800E86), numbers past the table (&800E85), C01-C76 through SharedCLibrary$Path, the two path "
          "variables, RISC_OSLib's messages; the help string ROSGD native's",
          "%s%s (%u lines the same, %u the house's)", v ? err(r)->errmess : "", diff, lines, native);
    ros_hostfs_unmount("SCLTest");
    snprintf(p, sizeof p, "%s/sclib,fff", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/Run,feb", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/out,ffd", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/out", root);
    unlink(p);
    rmdir(root);
}

/* ---- registration, from native code ------------------------------------------------------ */

#if ROS_CAPP_NATIVE

/* A client of the library made in the RMA: 8-byte slots and their tables
 * for chunks 1 and 2, its image's statics with a region for each chunk
 * (the block's layout), and descriptors naming them */
struct client {
    uint32_t mem, desc, stubs, statics;
};

static int make_client(struct client *c, const struct ros_clib_header *h, uint32_t header)
{
    uint32_t n1 = 0, n2 = 0;
    for (uint32_t i = 0; i < h->chunks; i++) {
        struct ros_clib_chunk k;
        memcpy(&k, ros_ptr(header + sizeof *h + i * sizeof k), sizeof k);
        if (k.id == 1)
            n1 = (k.entries_end - k.entries) / 4;
        if (k.id == 2)
            n2 = (k.entries_end - k.entries) / 4;
    }
    uint32_t stubs = 16 * (n1 + n2), size = 64 + stubs + h->block_size + 64;
    unsigned char *m = ros_rma_alloc(size + 16);
    if (!m || !n1 || !n2)
        return 0;
    memset(m, 0, size + 16);
    c->mem = ros_addr(m);
    c->desc = (c->mem + 15) & ~15u;
    c->stubs = c->desc + 64;
    c->statics = (c->stubs + stubs + 15) & ~15u;
    uint32_t e1 = c->stubs, e2 = c->stubs + 16 * n1, s1 = c->statics, s2 = c->statics + 2048;
    uint32_t d[11] = { 1, e1, e1 + 8 * n1, s1, s1 + 2048, 2, e2, e2 + 8 * n2, s2, s2 + h->tp_offset - 2048,
                       0xFFFFFFFFu };
    memcpy(ros_ptr(c->desc), d, sizeof d);
    return 1;
}

static void registration(void)
{
    uint32_t header = ros_sharedclib_image();
    if (!header) {
        check(0, "SharedCLibrary: no image mapped", NULL);
        return;
    }
    struct ros_clib_header h;
    memcpy(&h, ros_ptr(header), sizeof h);
    uint64_t gs0 = ros_capp_gs();
    struct client c;
    void *wmem = ros_rma_alloc(0x20000);
    uint32_t work = ros_addr(wmem);
    if (h.tp_offset != 8192 || !wmem || !make_client(&c, &h, header)) {
        check(0, "SharedCLibrary: the test's client", "block &%X", h.tp_offset);
        if (wmem)
            ros_rma_free(wmem);
        return;
    }
    work = (work + 15) & ~15u;
    /* The client's own statics: its image's [statics, +block) copied to the
     * workspace, 64 bytes past it zeroed (R3 the copy's end, R5 64 above);
     * the offset R1 - R4, so the library's template goes into the copy */
    uint32_t r4 = c.statics, r3 = c.statics + h.block_size, r5 = r3 + 64;
    memset(ros_ptr(work), 0xAA, 0x20000 - 32);
    ros_st32(r4 + h.tp_offset + 8, 0x12345678u);    /* a word of its own: TP+8, which the library leaves */
    uint32_t r[10] = { c.desc, work, work + 0x18000, r3, r4, r5, 4 << 16 | 1 };
    int v = swi(XSharedCLibrary_LibInitAPCS_32, r);
    uint32_t off = work - r4, tp = c.statics + off + h.tp_offset;
    /* __huge_val, chunk 2's template, the contract's TP-&1470: DBL_MAX */
    uint64_t huge = ros_ld64(tp - 0x1470);
    int zeroed = 1;
    for (uint32_t a = work + h.block_size; a < work + h.block_size + 64; a++)
        zeroed &= ros_ld8(a) == 0;
    check(!v && r[1] == work + (r5 - r4) && r[2] == r[1] + 4096 && r[0] == work + 0x18000 && r[6] == 6 &&
              ros_ld32(tp) == tp && ros_ld32(tp + 4) == r[1] && huge == 0x7FEFFFFFFFFFFFFFull && zeroed &&
              ros_ld32(tp + 8) == 0x12345678u && ros_ld64(r4 + 2960) == 0 &&
              ros_capp_gs() == tp,
          "SharedCLibrary: a client's own statics (R5 >= R4) copied and the rest zeroed, the library's "
          "template then put in the copy, not the image; the stack above them; TP the copy's top, the base",
          "V%d R0 &%X R1 &%X R2 &%X R6 %u, TP &%X self &%X limit &%X, __huge_val &%llX, gs &%llX", v, r[0], r[1],
          r[2], r[6], tp, ros_ld32(tp), ros_ld32(tp + 4), (unsigned long long)huge,
          (unsigned long long)ros_capp_gs());

    /* Chunk 2's slots not whole: C02.  Descriptors in no memory: &411. */
    ros_st32(c.desc + 20 + 8, ros_ld32(c.desc + 20 + 8) - 4);
    uint32_t q[10] = { c.desc, work, work + 0x18000, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 4 << 16 | 1 };
    int v2 = swi(XSharedCLibrary_LibInitAPCS_32, q);
    uint32_t e2 = v2 ? err(q)->errnum : 0;
    uint32_t z[10] = { ROS_SCREEN_BASE - 0x1000, work, work + 0x18000, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 4 << 16 | 1 };
    int v3 = swi(XSharedCLibrary_LibInitAPCS_32, z);
    check(v2 && e2 == 0x800E81 && !strcmp(err(q)->errmess, "Unknown library chunk") && v3 &&
              err(z)->errnum == 0x411,
          "SharedCLibrary: a chunk that is not whole 8-byte slots is C02; stub descriptors where no memory "
          "is, &411 before anything is written", "&%X, &%X", e2, v3 ? err(z)->errnum : 0);
    /* The registers an error returns (s/initmodule): R1 and R2 the stack's
     * base and top from C02 on (stored before the chunks are looked at);
     * with C01 as they came in, and R6 the root stack's size in bytes */
    uint32_t w1[10] = { c.desc, work, work + 100, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 4 << 16 | 1 };
    int v4 = swi(XSharedCLibrary_LibInitAPCS_32, w1);
    check(v2 && q[1] == work && q[2] == work + 4096 && v4 && err(w1)->errnum == 0x800E80 && w1[1] == work &&
              w1[2] == work + 100 && w1[6] == 4096,
          "SharedCLibrary: an error from C02 on returns R1/R2 the stack's base and top; C01 returns them "
          "as they came, R6 the root stack's size in bytes (s/initmodule's Failed)",
          "C02: R1 &%X R2 &%X; C01 V%d R1 &%X R2 &%X R6 %u", q[1], q[2], v4, w1[1], w1[2], w1[6]);
    /* A client of the kernel chunk alone registers, but has no TP, because
     * the block's top is past its region. So nothing is written there and
     * the base is left as it was. */
    uint32_t sentinel = c.statics + h.tp_offset;
    ros_st32(c.desc + 20, 0xFFFFFFFFu);         /* the list: chunk 1, then the end */
    ros_st32(sentinel, 0x5EA15EA1u);
    ros_capp_task_base(0x1230);
    uint32_t k[10] = { c.desc, work, work + 0x18000, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 4 << 16 | 1 };
    int v5 = swi(XSharedCLibrary_LibInitAPCS_32, k);
    check(!v5 && k[6] == 6 && ros_ld32(sentinel) == 0x5EA15EA1u && ros_capp_gs() == 0x1230,
          "SharedCLibrary: a client of the kernel chunk alone registers with no thread pointer -- nothing "
          "written above its region, the base left alone",
          "V%d R6 %u, the word at TP &%X, gs &%llX", v5, k[6], ros_ld32(sentinel),
          (unsigned long long)ros_capp_gs());
    ros_capp_task_base((uint32_t)gs0);
    ros_rma_free(ros_ptr(c.mem));
    ros_rma_free(wmem);
}

#endif

void ros_selftest_sclib(void)
{
    surface();
    paths();
#if ROS_CAPP_NATIVE
    registration();
#else
    uint32_t r[10] = { 0 };
    int v = swi(XSharedCLibrary_LibInitAPCS_32, r);
    check(v && err(r)->errnum == ROS_ERR_UNIMPLEMENTED && strstr(err(r)->errmess, "hosted build"),
          "SharedCLibrary, hosted: registration refused plainly -- no C library image, no 32-bit-pointer "
          "code", "%s", v ? err(r)->errmess : "registered");
#endif
}
