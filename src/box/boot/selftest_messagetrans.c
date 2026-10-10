/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_messagetrans.c: ResourceFS and MessageTrans, against RISC OS's
 * own (FileSys/ResourceFS, Internat/MsgTrans). They are used through the
 * SWIs, with a messages file of the test's own registered in ResourceFS.
 */
#include <string.h>

#include "resourcefs.h"
#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/rma.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

static const char test_messages[] =
    "# a test file\n"
    "Plain:Hello\n"
    "Two/Deux:Alternatives\n"
    "Line1\n"
    "Line2:Across lines\n"
    "Wild?:Wildcard %0\n"
    "Par:A%0B%1C%2D%3E%%F%9\n"
    "GS:Limit <Sys$RCLimit>\n"
    "Last:no newline";

static uint32_t text;               /* arena scratch */

static int swi(uint32_t n, uint32_t r[8])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 8 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 8 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[8])
{
    return ((os_error *)ros_ptr(r[0]))->errnum;
}

static const char *errmess(const uint32_t r[8])
{
    return ((os_error *)ros_ptr(r[0]))->errmess;
}

static uint32_t str(uint32_t at, const char *s)
{
    strcpy(ros_ptr(at), s);
    return at;
}

/* A ResourceFS block of one file, as tools/mkresources.py lays one out. */
static uint32_t make_block(const char *name, const char *data, uint32_t size)
{
    uint32_t n = (uint32_t)strlen(name) + 1, nlen = (n + 3) & ~3u, dlen = (size + 3) & ~3u;
    uint32_t entry = 20 + nlen + 4 + dlen;
    uint8_t *b = ros_rma_alloc(entry + 4);
    memset(b, 0, entry + 4);
    uint32_t a = ros_addr(b);
    ros_st32(a, entry);
    ros_st32(a + 4, 0xFFFFFF00u);
    ros_st32(a + 12, size);
    ros_st32(a + 16, 0x11);
    memcpy(b + 20, name, n);
    ros_st32(a + 20 + nlen, size + 4);
    memcpy(b + 24 + nlen, data, size);
    return a;
}

/* Lookup: token, into buf of size (0: in place), params. */
static int lookup(uint32_t desc, const char *token, uint32_t buf, uint32_t size, uint32_t r[8],
                  uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3)
{
    uint32_t tok = str(text + 256, token);
    uint32_t q[8] = { desc, tok, buf, size, p0, p1, p2, p3 };
    int v = swi(0x61502, q);
    memcpy(r, q, sizeof q);
    return v;
}

static int says(uint32_t desc, const char *token, const char *want)
{
    uint32_t r[8], buf = text + 512;
    int v = lookup(desc, token, buf, 200, r, 0, 0, 0, 0);
    int ok = !v && r[2] == buf && r[3] == strlen(want) && strcmp(ros_ptr(buf), want) == 0;
    if (!ok)
        ros_console_printf("        %s: V%d \"%s\" R3 %u\n", token, v,
                           v ? errmess(r) : (const char *)ros_ptr(buf), r[3]);
    return ok;
}

void ros_selftest_messagetrans(void)
{
    uint32_t r[8];
    text = ros_addr(ros_rma_alloc(1024));
    ros_callbacks_run();                /* ResourceFS's start-up services */

    /* ---- ResourceFS ---- */
    uint32_t global = ros_resourcefs_find("Resources:$.Resources.Global.Messages");
    int f1 = global && ros_resourcefs_find("resources:$.resources.GLOBAL.messages") == global &&
             ros_resourcefs_find("Resources:$.Resources.Nope") == 0;
    uint32_t block = make_block("Test.Msgs", test_messages, sizeof test_messages - 1);
    uint32_t q[8] = { block, 0, 0, 0, 0, 0, 0, 0 };
    int f2 = !swi(0x61B40, q) && ros_resourcefs_find("Resources:$.Test.Msgs") == block + 32;
    q[0] = block;
    int f3 = swi(0x61B40, q) && errnum(q) == 0x12E00;
    str(text, "TestMT$Path");
    str(text + 64, "Resources:$.Nowhere., Resources:$.Test.\r");
    uint32_t sv[8] = { text, text + 64, 1, 0, 0, 0, 0, 0 };
    swi(XOS_SetVarVal, sv);
    int f4 = ros_resourcefs_find("TestMT:Msgs") == block + 32;
    check(f1 && f2 && f3 && f4,
          "ResourceFS -- the ROM's files; registered and found without case, through path "
          "variables; &12E00", NULL);

    /* ---- FileInfo, OpenFile: a proxy in the RMA, the data where it lies ---- */
    uint32_t name = str(text + 128, "TestMT:Msgs");
    uint32_t fi[8] = { 0, name, 0, 0, 0, 0, 0, 0 };
    int o1 = !swi(0x61500, fi) && fi[0] == 1 && fi[2] == sizeof test_messages - 1 + 4;
    uint32_t desc = ros_addr(ros_rma_alloc(16));
    uint32_t of[8] = { desc, name, 0, 0, 0, 0, 0, 0 };
    int o2 = !swi(0x61501, of);
    uint32_t proxy = ros_ld32(desc + 8);
    int o3 = ros_ld32(desc) == 0x54534146u && (ros_ld32(desc + 4) & 0x40000000u) &&
             ros_ld32(proxy + 8) == block + 32 && ros_ld32(proxy + 20) == desc;
    check(o1 && o2 && o3,
          "MessageTrans_FileInfo, OpenFile -- flags and size; \"FAST\", a proxy, the file in "
          "ResourceFS", NULL);

    /* ---- Lookup ---- */
    int l1 = says(desc, "Plain", "Hello") && says(desc, "Deux", "Alternatives") &&
             says(desc, "Line1", "Across lines") && says(desc, "NoMem", "Not enough memory");
    lookup(desc, "Plain,rest", 0, 0, r, 0, 0, 0, 0);
    int l2 = ros_ld8(r[1]) == ',' && r[3] == 5 && memcmp(ros_ptr(r[2]), "Hello\n", 6) == 0 &&
             r[0] == desc;
    uint32_t p1 = str(text + 300, "1"), p3 = str(text + 310, "3"), p4 = str(text + 320, "4");
    lookup(desc, "Par", text + 512, 200, r, p1, 0, p3, p4);
    int l3 = strcmp(ros_ptr(text + 512), "A1B%1C3D4E%F%9") == 0 && r[3] == 14;
    uint32_t yes = str(text + 330, "yes");
    lookup(desc, "WildZ", text + 512, 200, r, yes, 0, 0, 0);
    int l4 = strcmp(ros_ptr(text + 512), "Wildcard yes") == 0;
    lookup(desc, "Plain", text + 512, 4, r, 0, 0, 0, 0);
    int l5 = strcmp(ros_ptr(text + 512), "Hel") == 0 && r[3] == 3;
    check(l1 && l2 && l3 && l4 && l5,
          "MessageTrans_Lookup -- alternatives by / and by line, the Global file after, in "
          "place, %0-%3 and %%, ? wildcards, truncation", NULL);

    int v = lookup(desc, "Nope", text + 512, 200, r, 0, 0, 0, 0);
    int n1 = v && errnum(r) == 0xAC2 && strcmp(errmess(r), "Message token Nope not found") == 0;
    lookup(desc, "Nope:Fallback %0", text + 512, 200, r, yes, 0, 0, 0);
    int n2 = strcmp(ros_ptr(text + 512), "Fallback yes") == 0;
    v = lookup(desc, "Last", text + 512, 200, r, 0, 0, 0, 0);
    int n3 = v && errnum(r) == 0xAC2;
    check(n1 && n2 && n3,
          "MessageTrans_Lookup -- not found &AC2 from its own Messages; token:default; the "
          "last line needs its newline", "%s", errmess(r));

    /* ---- ErrorLookup, CopyError, GSLookup ---- */
    ros_st32(text + 400, 0x123);
    str(text + 404, "Par");
    uint32_t el[8] = { text + 400, desc, 0, 0, p1, 0, p3, p4 };
    v = swi(0x61506, el);
    int e1 = v && errnum(el) == 0x123 && strcmp(errmess(el), "A1B%1C3D4E%F%9") == 0 &&
             el[0] != text + 400;
    uint32_t first = 0, distinct = 1;
    for (int i = 0; i < 16; i++) {
        uint32_t ce[8] = { text + 400, 0, 0, 0, 0, 0, 0, 0 };
        swi(0x61508, ce);
        if (i == 0)
            first = ce[0];
        else if (ce[0] == first)
            distinct = 0;
    }
    uint32_t ce[8] = { text + 400, 0, 0, 0, 0, 0, 0, 0 };
    v = swi(0x61508, ce);
    int e2 = v && distinct && ce[0] == first && strcmp(errmess(ce), "Par") == 0;
    uint32_t gs[8] = { desc, str(text + 256, "GS"), text + 512, 200, 0, 0, 0, 0 };
    v = swi(0x61507, gs);
    int e3 = !v && gs[2] == text + 512 && strncmp(ros_ptr(text + 512), "Limit 256", 9) == 0;
    check(e1 && e2 && e3,
          "MessageTrans_ErrorLookup, CopyError -- the 16 buffers in turn; GSLookup GSTranses",
          NULL);

    /* ---- EnumerateTokens ---- */
    char seen[64] = "";
    uint32_t place = 0;
    for (int i = 0; i < 6; i++) {
        uint32_t et[8] = { desc, str(text + 256, "Line*"), text + 512, 32, place, 0, 0, 0 };
        swi(0x61505, et);
        if (!et[2])
            break;
        strcat(seen, ros_ptr(text + 512));
        strcat(seen, ";");
        place = et[4];
    }
    uint32_t et[8] = { desc, str(text + 256, "?ar"), text + 512, 32, 0, 0, 0, 0 };
    swi(0x61505, et);
    int t1 = strcmp(seen, "Line1;Line2;") == 0 && et[2] && strcmp(ros_ptr(text + 512), "Par") == 0;
    uint32_t bad[8] = { desc, str(text + 256, "L*x"), text + 512, 32, 0, 0, 0, 0 };
    int t2 = swi(0x61505, bad) && errnum(bad) == 0xAC0;
    check(t1 && t2, "MessageTrans_EnumerateTokens -- ? and a final *, in order; &AC0 otherwise",
          "\"%s\"", seen);

    /* ---- MakeMenus ---- */
    uint8_t *def = ros_ptr(text + 600);
    memset(def, 0, 64);
    memcpy(def, "Plain", 6);                        /* title, and its terminator */
    memcpy(def + 6, "\x07\x02\x07\x00\x2C\x00", 6); /* colours, height, gap */
    memcpy(def + 12, "Two", 4);
    ros_st32(text + 616, 0x80);                     /* the last item */
    ros_st32(text + 620, 0);
    ros_st32(text + 624, 0x07000021);               /* icon flags: not indirected */
    ros_st8(text + 628, 0);
    uint32_t mm[8] = { desc, text + 600, text + 700, 100, 0, 0, 0, 0 };
    v = swi(0x61503, mm);
    int k1 = !v && strcmp(ros_ptr(text + 700), "Hello") == 0 && ros_ld8(text + 712) == 7 &&
             ros_ld32(text + 720) == 44 && ros_ld32(text + 728) == 0x80 &&
             strcmp(ros_ptr(text + 740), "Alternative") == 0 &&
             ros_ld32(text + 716) == 11 * 16 + 12 && mm[2] == text + 700 + 28 + 24;
    check(k1, "MessageTrans_MakeMenus -- the title, colours, items, the width from the widest",
          NULL);

    /* ---- the client's own buffer, CloseFile ---- */
    uint32_t buf = ros_addr(ros_rma_alloc(fi[2]));
    uint32_t desc2 = ros_addr(ros_rma_alloc(16));
    uint32_t of2[8] = { desc2, name, buf, 0, 0, 0, 0, 0 };
    int c1 = !swi(0x61501, of2) && ros_ld32(desc2 + 8) == buf && ros_ld32(buf) == fi[2] &&
             says(desc2, "Plain", "Hello");
    uint32_t cf[8] = { desc, 0, 0, 0, 0, 0, 0, 0 };
    int c2 = !swi(0x61504, cf) && ros_ld32(desc) == 0 &&
             lookup(desc, "Plain", text + 512, 200, r, 0, 0, 0, 0) && errnum(r) == 0xAC2;
    uint32_t bs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    int c3 = swi(0x61520, bs) && errnum(bs) == 0x1E6 &&
             strcmp(errmess(bs), "SWI value out of range for module MessageTrans") == 0;
    check(c1 && c2 && c3,
          "MessageTrans -- a file in the client's buffer; CloseFile, then not found; &1E6", NULL);

    /* ---- ResourceFS changes under an open file ---- */
    uint32_t desc3 = ros_addr(ros_rma_alloc(16));
    uint32_t of3[8] = { desc3, name, 0, 0, 0, 0, 0, 0 };
    swi(0x61501, of3);
    q[0] = block;
    int g1 = !swi(0x61B41, q) && swi(0x61B41, q) && errnum(q) == 0x12E01;
    ros_callbacks_run();                /* Service_ResourceFSStarted */
    uint32_t pr = ros_ld32(desc3 + 8);
    int g2 = ros_ld32(pr + 8) == 0 && lookup(desc3, "Plain", text + 512, 200, r, 0, 0, 0, 0) &&
             errnum(r) == 0xAC2 && says(desc3, "NoMem", "Not enough memory");
    uint32_t cf3[8] = { desc3, 0, 0, 0, 0, 0, 0, 0 };
    swi(0x61504, cf3);
    check(g1 && g2,
          "ResourceFS deregistered, &12E01 twice: an open file is let go, the Global file stays",
          NULL);

    str(text, "TestMT$Path");
    uint32_t us[8] = { text, 0, (uint32_t)-1, 0, 0, 0, 0, 0 };
    swi(XOS_SetVarVal, us);
    ros_rma_free(ros_ptr(desc3));
    ros_rma_free(ros_ptr(desc2));
    ros_rma_free(ros_ptr(buf));
    ros_rma_free(ros_ptr(desc));
    ros_rma_free(ros_ptr(block));
    ros_rma_free(ros_ptr(text));
}
