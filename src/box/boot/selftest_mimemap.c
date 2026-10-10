/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_mimemap.c: MimeMap, native (modules/mimemap, #106): what must
 * always hold. The module finds the ROM's MIME mappings file with no
 * !Boot (InetDBase$Path and Inet$MimeMappings are set as it starts). The
 * ROM's table types every extension that FileSwitch's typemap.txt knows as
 * HostFS types it, except where RISC OS 5.30's table has its own mapping.
 * A few conversions are checked each way. tests/desktop/mimemap holds the
 * rest, every conversion against the farm's 5.30.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "mimemap.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

/* HostFS's extension to file type table (tools/mktypemap.py) */
extern const struct ros_typemap_entry {
    const char *ext;
    uint16_t type;
} ros_typemap[];
extern const unsigned ros_typemap_count;

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

static char *block;                     /* arena: the input, then the output */

/* MimeMap_Translate: 0 and the output result (R3 for a file type, else the
 * output's text), or the error's number */
static uint32_t translate(uint32_t from, const char *in, uint32_t in_type, uint32_t to,
                          uint32_t *type, char *out, size_t max)
{
    if (in)
        strcpy(block, in);
    uint32_t r[8] = { from, in ? ros_addr(block) : in_type, to, ros_addr(block + 256) };
    if (swi(ROS_X_BIT | 0x50B00u, r))
        return ((const os_error *)ros_ptr(r[0]))->errnum;
    if (type)
        *type = r[3];
    if (out)
        snprintf(out, max, "%s", block + 256);
    return 0;
}

static void var(const char *name, char *out, size_t max)
{
    strcpy(block, name);
    uint32_t r[8] = { ros_addr(block), ros_addr(block + 256), 255, 0, 3 };
    out[0] = 0;
    if (!swi(XOS_ReadVarVal, r))
        snprintf(out, max, "%.*s", (int)r[2], block + 256);
}

void ros_selftest_mimemap(void)
{
    block = ros_rma_alloc(512);
    if (!block) {
        check(0, "MimeMap: room for the self-test", "no RMA");
        return;
    }

    /* The ROM's table, found with no !Boot */
    char dbase[256], mappings[256];
    var("InetDBase$Path", dbase, sizeof dbase);
    var("Inet$MimeMappings", mappings, sizeof mappings);
    uint32_t png = 0, cab = 0, cal = 0;
    char mime[64] = "", ext[64] = "";
    int ok = translate(MMM_TYPE_DOT_EXTN, ".png", 0, MMM_TYPE_RISCOS, &png, NULL, 0) == 0 &&
             translate(MMM_TYPE_DOT_EXTN, "cab", 0, MMM_TYPE_RISCOS, &cab, NULL, 0) == 0 &&
             translate(MMM_TYPE_MIME, "text/calendar", 0, MMM_TYPE_RISCOS, &cal, NULL, 0) == 0 &&
             translate(MMM_TYPE_DOT_EXTN, ".c", 0, MMM_TYPE_MIME, NULL, mime, sizeof mime) == 0 &&
             translate(MMM_TYPE_RISCOS, NULL, 0xB60, MMM_TYPE_DOT_EXTN, NULL, ext, sizeof ext) == 0;
    check(ok && !strcmp(dbase, MIMEMAP_ROM_DBASE) && !strcmp(mappings, MIMEMAP_DEFAULT) &&
          png == 0xB60 && cab == 0xABF && cal == 0x1D5 && !strcmp(mime, "text/plain") &&
          !strcmp(ext, "png"),
          "MimeMap: the ROM's table found with no !Boot (InetDBase$Path, Inet$MimeMappings); .png &B60, "
          "cab &ABF, text/calendar &1D5, .c text/plain, &B60 png",
          "ok %d, InetDBase$Path '%s', Inet$MimeMappings '%s', png &%X, cab &%X, calendar &%X, .c '%s', "
          "&B60 '%s'", ok, dbase, mappings, png, cab, cal, mime, ext);

    /* Every extension HostFS types is typed alike by MimeMap. The exceptions are
     * those that 5.30's table maps itself, whose mapping stands, and
     * typemap.txt's "z1-z8", which is a range and not an extension. */
    static const struct { const char *ext; uint32_t type; } own[] = {
        { "ltx", 0x2A8 }, { "json", 0xF75 }, { "yaml", 0xF74 }, { "yml", 0xF74 },
    };
    unsigned n = 0, bad = 0;
    char first[96] = "";
    for (unsigned i = 0; i < ros_typemap_count; i++) {
        const char *e = ros_typemap[i].ext;
        if (strchr(e, '-'))
            continue;
        uint32_t want = ros_typemap[i].type, got = 0;
        for (unsigned j = 0; j < sizeof own / sizeof own[0]; j++)
            if (!strcasecmp(own[j].ext, e))
                want = own[j].type;
        uint32_t err = translate(MMM_TYPE_DOT_EXTN, e, 0, MMM_TYPE_RISCOS, &got, NULL, 0);
        n++;
        if (err || got != want) {
            if (!bad++)
                snprintf(first, sizeof first, ".%s: &%X, error &%X, not &%X", e, got, err, want);
        }
    }
    check(n > 150 && !bad,
          "MimeMap: every extension typemap.txt types (HostFS's), the same type from the ROM's table, "
          "but where 5.30's own mapping stands (.ltx, .json, .yaml, .yml)",
          "%u of %u wrong; first %s", bad, n, first);

    /* 5.30's errors */
    uint32_t e1 = translate(MMM_TYPE_DOT_EXTNS, ".png", 0, MMM_TYPE_RISCOS, NULL, NULL, 0);
    uint32_t e2 = translate(MMM_TYPE_DOT_EXTN, ".nosuch", 0, MMM_TYPE_RISCOS, NULL, NULL, 0);
    check(e1 == 0xB00001 && e2 == 0xB00002,
          "MimeMap: an input format with no lookup &B00001, no mapping &B00002",
          "&%X, &%X", e1, e2);
    ros_rma_free(block);
}
