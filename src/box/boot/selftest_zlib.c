/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_zlib.c: ZLib, native over zlib (modules/zlibmod): what must
 * always hold. The values marked "5.30" are what the farm's ZLib 0.05
 * gave for the same calls (tests/desktop/compress has the probes, register
 * by register). They are its bytes, checksums, sizes, codes, messages and
 * errors, and a gzip file with RISC OS's header byte for byte. Round trips
 * are made through every way in: Squash's interface a little at a time, the
 * stream calls with gzip, raw and zlib wrappers, copies, dictionaries and
 * resets, the one-shot calls, and gzip files read, written and sought.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/module.h"
#include "rosgd/rma.h"
#include "fileswitch.h"
#include "selftest.h"

#define check ros_check

/* A scratch disc of the test's own, as selftest_files makes its "Rec":
 * a fresh directory (TMPDIR hosted, /tmp in the box), mounted as name;
 * 0 if there is none */
static int scratch_disc(const char *name, char *dir, size_t max)
{
    const char *tmp = getenv("TMPDIR");
    if (!(tmp && *tmp))
        tmp = "/tmp";
    mkdir(tmp, 0777);
    snprintf(dir, max, "%s/rosgd-%s-XXXXXX", tmp, name);
    if (!mkdtemp(dir))
        return 0;
    return ros_hostfs_mount(name, dir) == 0;
}

static void scratch_gone(const char *name, const char *dir)
{
    ros_hostfs_unmount(name);
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[700];
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    if (d)
        closedir(d);
    rmdir(dir);
}

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
    return ((const os_error *)ros_ptr(r[0]))->errnum;
}

static const char *errmess(const uint32_t r[8])
{
    return ((const os_error *)ros_ptr(r[0]))->errmess;
}

static uint32_t xs(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    return x ^ x << 5;
}

static void text(uint8_t *p, uint32_t n)
{
    static const char *const words[8] = { "the ", "quick ", "brown ", "fox ", "RISC ", "OS ",
                                          "zlib ", ". " };
    uint32_t x = 0x9E3779B9u, i = 0;
    while (i < n) {
        x = xs(x);
        for (const char *w = words[x & 7]; *w && i < n; w++)
            p[i++] = (uint8_t)*w;
    }
}

/* An arena string */
static uint32_t str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = ros_rma_alloc((uint32_t)n);
    memcpy(p, s, n);
    return ros_addr(p);
}

static struct ros_module *find(const char *title)
{
    for (struct ros_module *m = ros_module_first(); m; m = m->next)
        if (!strcmp(m->title, title))
            return m;
    return NULL;
}

/* The farm's: ZLib_Compress of the 1000 bytes i MOD 7 + 65 */
static const uint8_t farm_compress[23] = {
    0x78, 0xDA, 0x73, 0x74, 0x72, 0x76, 0x71, 0x75, 0x73, 0x77, 0x1C, 0xA5,
    0x46, 0xA9, 0x51, 0x6A, 0xF8, 0x52, 0x00, 0x63, 0xFD, 0x09, 0xAD,
};

/* The farm's gzip file: "wb9R", load &FFFFFD12, exec &34567890, length
 * 10, attributes 3; 100 bytes of digits, then a seek on to 150 */
static const uint8_t farm_gz[70] = {
    0x1f, 0x8b, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0d, 0x20, 0x00, 0x41, 0x43,
    0x1c, 0x00, 0x12, 0xfd, 0xff, 0xff, 0x90, 0x78, 0x56, 0x34, 0x03, 0x00, 0x00, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x33, 0x30, 0x34, 0x32, 0x36, 0x31, 0x35, 0x33, 0xb7, 0xb0, 0x34, 0xa0,
    0x19, 0x8b, 0x81, 0x64, 0x00, 0x00, 0x7b, 0xc3, 0xfc, 0xbf, 0x96, 0x00, 0x00, 0x00,
};

/* Squash's interface, a little at a time: in/out per call; the bytes out */
static uint32_t pieces(uint32_t swin, uint32_t ws, const uint8_t *in, uint32_t n, uint8_t *out,
                       uint32_t room, uint32_t step_in, uint32_t step_out, int *ok)
{
    uint32_t done_in = 0, done_out = 0, flags = 0;
    *ok = 0;
    for (int calls = 0; calls < 100000; calls++) {
        uint32_t give = n - done_in < step_in ? n - done_in : step_in;
        uint32_t space = room - done_out < step_out ? room - done_out : step_out;
        uint32_t more = done_in + give < n ? 2u : 0u;
        uint32_t r[8] = { flags | more, ws, ros_addr(in + done_in), give, ros_addr(out + done_out), space };
        if (swi(swin, r))
            return 0;
        done_in += give - r[3];
        done_out += space - r[5];
        flags = 1;
        if (r[0] == 0) {
            *ok = ros_ld32(ws + 28) == 0;   /* the stream let go */
            return done_out;
        }
    }
    return 0;
}

void ros_selftest_zlib(void)
{
    struct ros_module *m = find("ZLib");
    uint32_t r[8] = { 0, str("ZLib_InflateGetDictionary") };
    int names = !swi(XOS_SWINumberFromString, r) && r[0] == 0x53AE5;
    uint32_t r2[8] = { 0, str("ZLib_TaskAssociate") };
    names = names && !swi(XOS_SWINumberFromString, r2) && r2[0] == 0x53AE0;
    uint32_t v[8] = { 0 };
    int ver = !swi(XZLib_Version, v) && ros_ld8(v[0]) == '1';
    check(m && ros_module_version(m) == 0x500 && m->swi_chunk == 0x53AC0 && m->swi_count == 38 &&
          names && ver,
          "ZLib 0.05: its 38 SWIs at &53AC0 by name, as 5.30's, and zlib's version", NULL);

    /* sizes and flags (5.30's) */
    uint32_t a[8] = { 8, 0xFFFFFFFFu }, b[8] = { 8, 1000 }, c[8] = { 8, 1000 };
    uint32_t d[8] = { 0x20, 1000 }, e[8] = { 0x20, 1000 }, f[8] = { 0x04 | 8, 7 };
    uint32_t f2[8] = { 0x04 | 8, 7 };
    int sizes = !swi(XZLib_Compress, a) && a[0] == 56 && a[1] == 0xFFFFFFFFu &&
                !swi(XZLib_Compress, b) && b[0] == 56 && b[1] == 1013 &&
                !swi(XZLib_Decompress, c) && c[0] == 56 && c[1] == 0xFFFFFFFFu &&
                swi(XZLib_Compress, d) && errnum(d) == 0x81F001 &&
                !strcmp(errmess(d), "Invalid ZLib_Compress flag") &&
                swi(XZLib_Decompress, e) && errnum(e) == 0x81F002 &&
                swi(XZLib_Compress, f) && errnum(f) == 0x81F001 &&
                !swi(XZLib_Decompress, f2) && f2[0] == 56;   /* bit 2 allowed there */
    check(sizes, "ZLib_Compress and _Decompress's sizes (56, compressBound) and flags, as 5.30's", NULL);

    uint8_t *pat = ros_rma_alloc(1000), *out = ros_rma_alloc(220000), *back = ros_rma_alloc(110000);
    uint8_t *ws = ros_rma_alloc(56), *ws2 = ros_rma_alloc(56), *big = ros_rma_alloc(100000);
    if (!pat || !out || !back || !ws || !ws2 || !big) {
        check(0, "ZLib: RMA for the tests", NULL);
        return;
    }
    for (int i = 0; i < 1000; i++)
        pat[i] = (uint8_t)(i % 7 + 65);
    uint32_t pa = ros_addr(pat), oa = ros_addr(out), wa = ros_addr(ws);

    /* checksums: the standard check values, and 5.30's of the pattern */
    uint32_t nine = str("123456789"), wiki = str("Wikipedia");
    uint32_t k1[8] = { 0, nine, nine + 9 }, k2[8] = { 1, wiki, wiki + 9 };
    uint32_t k3[8] = { 0, pa, pa + 1000 }, k4[8] = { 1, pa, pa + 1000 }, k5[8] = { 77, 0, 0 };
    check(!swi(XZLib_CRC32, k1) && k1[0] == 0xCBF43926u && !swi(XZLib_Adler32, k2) &&
          k2[0] == 0x11E60398u && !swi(XZLib_CRC32, k3) && k3[0] == 0x988EFE5Bu &&
          !swi(XZLib_Adler32, k4) && k4[0] == 0x63FD09ADu && !swi(XZLib_CRC32, k5) && k5[0] == 0,
          "ZLib_CRC32 and _Adler32: the check values, and 5.30's", NULL);

    /* 5.30's bytes: ZLib_Compress at one go (level 9), ZCompress (6) */
    uint32_t z1[8] = { 0, wa, pa, 1000, oa, 2000 };
    int one = !swi(XZLib_Compress, z1) && z1[0] == 0 && z1[2] == pa + 1000 && z1[3] == 0 &&
              z1[4] - oa == 23 && z1[5] == 1977 && !memcmp(out, farm_compress, 23) &&
              ros_ld32(wa + 28) == 0;
    uint32_t z2[8] = { oa, 2000, pa, 1000 };
    int zc = !swi(XZLib_ZCompress, z2) && z2[0] == 0 && z2[1] == 23 && out[0] == 0x78 &&
             out[1] == 0x9C && !memcmp(out + 2, farm_compress + 2, 21);
    uint32_t z3[8] = { oa, 2000, pa, 1000, 1 }, z4[8] = { oa, 5, pa, 1000, 9 };
    uint32_t z5[8] = { pa, 1000, oa, 0 };
    zc = zc && !swi(XZLib_ZCompress2, z3) && z3[0] == 0 && z3[1] == 26 &&
         !swi(XZLib_ZCompress2, z4) && z4[0] == (uint32_t)-5 && z4[1] == 5 &&
         !swi(XZLib_ZUncompress, z5) && z5[0] == (uint32_t)-3 && z5[1] == 0;
    check(one && zc, "ZLib_Compress, _ZCompress and _ZCompress2 make 5.30's bytes; too little "
          "room is Z_BUF_ERROR, nothing to uncompress Z_DATA_ERROR, as there", NULL);

    /* round trips a little at a time */
    text(big, 100000);
    int ok1, ok2, ok3, ok4;
    uint32_t n1 = pieces(XZLib_Compress, wa, big, 100000, out, 220000, 777, 333, &ok1);
    uint32_t n2 = pieces(XZLib_Decompress, wa, out, n1, back, 110000, 100, 200, &ok2);
    uint32_t n3 = pieces(XZLib_Compress, wa, big, 100000, out, 220000, 100000, 7, &ok3);
    uint32_t n4 = pieces(XZLib_Decompress, wa, out, n3, back, 110000, 13, 100000, &ok4);
    check(n1 && n1 < 40000 && n2 == 100000 && !memcmp(back, big, 100000) && ok1 && ok2 &&
          n3 == n1 && n4 == 100000 && !memcmp(back, big, 100000) && ok3 && ok4,
          "ZLib_Compress and _Decompress a little at a time (777/333, 100/200, all/7, 13/all): "
          "100000 bytes there and back, the stream let go at the end", "%u %u %u %u", n1, n2, n3, n4);

    /* the stream calls */
    uint32_t vs = v[0];
    uint32_t i1[8] = { wa, 6, vs, 56 }, i2[8] = { wa, 6, vs, 60 }, i3[8] = { wa, 6, str("0.9"), 56 };
    int inits = !swi(XZLib_DeflateInit, i1) && i1[0] == 0 && ros_ld32(wa + 24) == 0 &&
                ros_ld32(wa + 28) != 0 && ros_ld32(wa + 44) == 2;
    uint32_t de[8] = { wa };
    inits = inits && !swi(XZLib_DeflateEnd, de) && de[0] == 0 && ros_ld32(wa + 28) == 0;
    inits = inits && !swi(XZLib_DeflateInit, i2) && i2[0] == (uint32_t)-6 &&
            !swi(XZLib_DeflateInit, i3) && i3[0] == (uint32_t)-6;
    uint32_t de2[8] = { wa };
    inits = inits && !swi(XZLib_DeflateEnd, de2) && de2[0] == (uint32_t)-2;
    uint32_t ii[8] = { wa, vs, 56 };
    uint32_t abcd = str("ABCD");
    int bad = !swi(XZLib_InflateInit, ii) && ii[0] == 0;
    ros_st32(wa + 0, abcd);
    ros_st32(wa + 4, 4);
    ros_st32(wa + 12, oa);
    ros_st32(wa + 16, 100);
    uint32_t inf[8] = { wa, 0 };
    bad = bad && !swi(XZLib_Inflate, inf) && inf[0] == (uint32_t)-3 && ros_ld32(wa + 24) &&
          !strcmp(ros_ptr(ros_ld32(wa + 24)), "incorrect header check") && ros_ld32(wa + 8) == 2 &&
          ros_ld32(wa + 20) == 0;
    uint32_t ie[8] = { wa };
    bad = bad && !swi(XZLib_InflateEnd, ie) && ie[0] == 0;
    check(inits && bad, "ZLib's inits: version and block size checked (Z_VERSION_ERROR), the block's "
          "msg, state and data_type as 5.30 leaves them; a bad header's message and totals", NULL);

    /* gzip through DeflateInit2, copied half way; InflateInit2 back */
    uint32_t g[8] = { wa, 9, 8, 31, 8, 0, vs, 56 };
    int gz = !swi(XZLib_DeflateInit2, g) && g[0] == 0;
    ros_st32(wa + 0, ros_addr(big));
    ros_st32(wa + 4, 50000);
    ros_st32(wa + 12, oa);
    ros_st32(wa + 16, 110000);
    uint32_t dd[8] = { wa, 0 };
    gz = gz && !swi(XZLib_Deflate, dd) && dd[0] == 0;
    uint32_t cp[8] = { ros_addr(ws2), wa };
    gz = gz && !swi(XZLib_DeflateCopy, cp) && cp[0] == 0 && ros_ld32(ros_addr(ws2) + 28) &&
         ros_ld32(ros_addr(ws2) + 28) != ros_ld32(wa + 28);
    uint32_t pend[8] = { wa, ros_addr(back), ros_addr(back) + 4 };
    gz = gz && !swi(XZLib_DeflatePending, pend) && pend[0] == 0;
    ros_st32(wa + 0, ros_addr(big) + 50000);
    ros_st32(wa + 4, 50000);
    uint32_t df[8] = { wa, 4 };
    gz = gz && !swi(XZLib_Deflate, df) && df[0] == 1;
    uint32_t glen = ros_ld32(wa + 20);
    /* the copy goes on into its own output, the same */
    uint8_t *out2 = ros_rma_alloc(110000);
    uint32_t w2 = ros_addr(ws2);
    uint32_t sofar = ros_ld32(w2 + 12) - oa;
    memcpy(out2, out, sofar);
    ros_st32(w2 + 12, ros_addr(out2) + sofar);
    ros_st32(w2 + 0, ros_addr(big) + 50000);
    ros_st32(w2 + 4, 50000);
    uint32_t df2[8] = { w2, 4 };
    gz = gz && out2 && !swi(XZLib_Deflate, df2) && df2[0] == 1 && ros_ld32(w2 + 20) == glen &&
         !memcmp(out, out2, glen) && out[0] == 0x1F && out[1] == 0x8B;
    uint32_t e1[8] = { wa }, e2[8] = { w2 };
    gz = gz && !swi(XZLib_DeflateEnd, e1) && !swi(XZLib_DeflateEnd, e2);
    uint32_t gi[8] = { wa, 31, vs, 56 };
    gz = gz && !swi(XZLib_InflateInit2, gi) && gi[0] == 0;
    ros_st32(wa + 0, oa);
    ros_st32(wa + 4, glen);
    ros_st32(wa + 12, ros_addr(back));
    ros_st32(wa + 16, 110000);
    uint32_t gin[8] = { wa, 4 };
    gz = gz && !swi(XZLib_Inflate, gin) && gin[0] == 1 && ros_ld32(wa + 20) == 100000 &&
         !memcmp(back, big, 100000) && ros_ld32(wa + 48) != 0;
    uint32_t r2a[8] = { wa, (uint32_t)-15 }, gie[8] = { wa };
    gz = gz && !swi(XZLib_InflateReset2, r2a) && r2a[0] == 0 && !swi(XZLib_InflateEnd, gie);
    check(gz, "ZLib's stream calls: gzip by DeflateInit2, a DeflateCopy half way finishing the "
          "same, DeflatePending, and InflateInit2 back to the 100000 bytes", NULL);

    /* a dictionary there and back; ZUncompress2 */
    uint32_t dict = str("the quick brown fox RISC OS zlib");
    uint32_t di[8] = { wa, 9, vs, 56 };
    int dic = !swi(XZLib_DeflateInit, di) && di[0] == 0;
    uint32_t sd[8] = { wa, dict, 32 };
    dic = dic && !swi(XZLib_DeflateSetDictionary, sd) && sd[0] == 0;
    uint32_t gl = ros_addr(back) + 200;
    ros_st32(gl, 0);
    uint32_t gd[8] = { wa, ros_addr(back), gl };
    dic = dic && !swi(XZLib_DeflateGetDictionary, gd) && gd[0] == 0 && ros_ld32(gl) == 32 &&
          !memcmp(back, ros_ptr(dict), 32);
    uint32_t pr[8] = { wa, 1, 0 };
    dic = dic && !swi(XZLib_DeflateParams, pr) && pr[0] == 0;
    ros_st32(wa + 0, ros_addr(big));
    ros_st32(wa + 4, 5000);
    ros_st32(wa + 12, oa);
    ros_st32(wa + 16, 10000);
    uint32_t dd2[8] = { wa, 4 };
    dic = dic && !swi(XZLib_Deflate, dd2) && dd2[0] == 1;
    uint32_t dlen = ros_ld32(wa + 20);
    uint32_t did[8] = { 1, dict, dict + 32 };
    dic = dic && !swi(XZLib_Adler32, did);
    uint32_t dre[8] = { wa };
    dic = dic && !swi(XZLib_DeflateReset, dre) && dre[0] == 0 && ros_ld32(wa + 20) == 0;
    uint32_t dend[8] = { wa };
    dic = dic && !swi(XZLib_DeflateEnd, dend);
    uint32_t ii2[8] = { wa, vs, 56 };
    dic = dic && !swi(XZLib_InflateInit, ii2);
    ros_st32(wa + 0, oa);
    ros_st32(wa + 4, dlen);
    ros_st32(wa + 12, ros_addr(back));
    ros_st32(wa + 16, 10000);
    uint32_t in1[8] = { wa, 0 };
    dic = dic && !swi(XZLib_Inflate, in1) && in1[0] == 2 && ros_ld32(wa + 48) == did[0];
    uint32_t isd[8] = { wa, dict, 32 };
    dic = dic && !swi(XZLib_InflateSetDictionary, isd) && isd[0] == 0;
    uint32_t in2[8] = { wa, 0 };
    dic = dic && !swi(XZLib_Inflate, in2) && in2[0] == 1 && ros_ld32(wa + 20) == 5000 &&
          !memcmp(back, big, 5000);
    uint32_t iend[8] = { wa };
    dic = dic && !swi(XZLib_InflateEnd, iend);
    uint32_t srcw = ros_addr(back) + 6000;
    ros_st32(srcw, dlen);
    uint32_t u2[8] = { ros_addr(back), 10000, oa, srcw };
    dic = dic && !swi(XZLib_ZUncompress2, u2) && u2[0] == (uint32_t)-3;    /* wants the dictionary */
    uint32_t zc9[8] = { oa, 10000, ros_addr(big), 5000 };
    dic = dic && !swi(XZLib_ZCompress, zc9) && zc9[0] == 0;
    uint32_t clen = zc9[1];
    ros_st32(srcw, clen + 50);
    uint32_t u3[8] = { ros_addr(back), 10000, oa, srcw };
    dic = dic && !swi(XZLib_ZUncompress2, u3) && u3[0] == 0 && u3[1] == 5000 && ros_ld32(srcw) == clen;
    check(dic, "ZLib's dictionaries (set, got back, needed: Z_NEED_DICT then the data), "
          "DeflateParams, DeflateReset; ZUncompress2 reports the input it used", NULL);

    /* the module's own errors (5.30's) */
    uint32_t ta[8] = { wa, 2 }, gs[8] = { 0, 0, 2 }, ta2[8] = { wa, 1 };
    struct ros_cpu s;
    ros_cpu_enter(&s);
    ros_swi(&s, 0x20000u | 0x53AE6u);
    int unknown = s.v && ((os_error *)ros_ptr(s.r[0]))->errnum == 0x81F004 &&
                  !strcmp(((os_error *)ros_ptr(s.r[0]))->errmess, "Unknown ZLib SWI");
    check(swi(XZLib_TaskAssociate, ta) && errnum(ta) == 0x81F005 &&
          !strcmp(errmess(ta), "Invalid ZLib_TaskAssociate operation") &&
          !swi(XZLib_TaskAssociate, ta2) && ta2[0] == wa &&
          swi(XZLib_GZSeek, gs) && errnum(gs) == 0x81F006 && unknown,
          "ZLib's errors: TaskAssociate's operation (&81F005), GZSeek's (&81F006), SWIs past "
          "the 38 (&81F004 Unknown ZLib SWI), as 5.30's", NULL);

    /* a block as both ends of DeflateCopy; DeflateParams with a bad buffer */
    {
        uint32_t blk = (uint32_t)ros_addr(ros_rma_alloc(56));
        for (uint32_t o = 0; o < 56; o += 4)
            ros_st32(blk + o, 0);
        uint32_t di[8] = { blk, 6, vs, 56 }, cp[8] = { blk, blk }, pr[8] = { blk, 9, 0 };
        int ok = !swi(XZLib_DeflateInit, di) && di[0] == 0 && !swi(XZLib_DeflateCopy, cp) &&
                 cp[0] == (uint32_t)-2;
        ros_st32(blk + 16, 64);                 /* avail_out, with next_out in no memory */
        ros_st32(blk + 12, 0xFFFFF000u);
        ok = ok && swi(XZLib_DeflateParams, pr) && errnum(pr) == 0x80000002u;
        ros_st32(blk + 16, 0);
        ros_st32(blk + 12, 0);
        uint32_t de3[8] = { blk };
        ok = ok && !swi(XZLib_DeflateEnd, de3) && de3[0] == 0;
        check(ok, "ZLib_DeflateCopy of a block onto itself is Z_STREAM_ERROR and leaves the stream "
              "alone; ZLib_DeflateParams checks the block's buffers as Deflate does", NULL);
    }

    /* gzip files: 5.30's file, byte for byte; read back with its metadata */
    char dir[600];
    if (!scratch_disc("ZTest", dir, sizeof dir)) {
        check(0, "ZLib's gzip files: a scratch disc", NULL);
        return;
    }
    uint32_t name = str("HostFS::ZTest.$.GZTest"), wmode = str("wb9R");
    uint32_t go[8] = { name, wmode, 0xFFFFFD12u, 0x34567890u, 10, 3 };
    int gzw = !swi(XZLib_GZOpen, go) && go[0] != 0 && go[2] == 0xFFFFFD12u && go[4] == 10;
    uint32_t h = go[0];
    for (int i = 0; i < 100; i++)
        back[i] = (uint8_t)('0' + i % 10);
    uint32_t gw[8] = { h, ros_addr(back), 100 }, gt[8] = { h }, sb[8] = { h, 50, 0 };
    uint32_t sf[8] = { h, 150, 0 }, gc[8] = { h };
    gzw = gzw && !swi(XZLib_GZWrite, gw) && gw[0] == 100 && !swi(XZLib_GZTell, gt) && gt[0] == 100 &&
          !swi(XZLib_GZSeek, sb) && sb[0] == 0xFFFFFFFFu && !swi(XZLib_GZSeek, sf) && sf[0] == 150 &&
          !swi(XZLib_GZClose, gc) && gc[0] == 0;
    uint32_t ld[8] = { 255, name, ros_addr(out), 0 }, rd[8] = { 17, name };
    gzw = gzw && !swi(XOS_File, rd) && rd[4] == 70 && !swi(XOS_File, ld) && !memcmp(out, farm_gz, 70);
    uint32_t ro[8] = { name, str("rbR"), 1, 2, 3, 4 };
    int gzr = !swi(XZLib_GZOpen, ro) && ro[0] && ro[2] == 0xFFFFFD12u && ro[3] == 0x34567890u &&
              ro[4] == 10 && ro[5] == 3;
    h = ro[0];
    uint32_t r40[8] = { h, ros_addr(out), 40 }, rel[8] = { h, 10, 1 }, abs5[8] = { h, 5, 0 };
    uint32_t rall[8] = { h, ros_addr(out), 1000 }, eof[8] = { h }, tl[8] = { h }, rc[8] = { h };
    gzr = gzr && !swi(XZLib_GZRead, r40) && r40[0] == 40 && !memcmp(out, back, 40) &&
          !swi(XZLib_GZSeek, rel) && rel[0] == 50 && !swi(XZLib_GZSeek, abs5) && abs5[0] == 5 &&
          !swi(XZLib_GZRead, rall) && rall[0] == 145 && !memcmp(out, back + 5, 95) &&
          out[95] == 0 && out[144] == 0 && !swi(XZLib_GZEOF, eof) && eof[0] == 1 &&
          !swi(XZLib_GZTell, tl) && tl[0] == 150 && !swi(XZLib_GZClose, rc) && rc[0] == 0;
    uint32_t none[8] = { str("HostFS::ZTest.$.NoSuchGZ"), str("rb") };
    uint32_t badm[8] = { name, str("q") };
    uint32_t plain[8] = { str("HostFS::ZTest.$.GZPlain"), str("wT") };
    int gzo = !swi(XZLib_GZOpen, none) && none[0] == 0 && !swi(XZLib_GZOpen, badm) && badm[0] == 0 &&
              !swi(XZLib_GZOpen, plain) && plain[0];
    uint32_t pw[8] = { plain[0], ros_addr(back), 30 }, pc[8] = { plain[0] };
    gzo = gzo && !swi(XZLib_GZWrite, pw) && pw[0] == 30 && !swi(XZLib_GZClose, pc);
    uint32_t pr2[8] = { str("HostFS::ZTest.$.GZPlain"), str("r") };
    gzo = gzo && !swi(XZLib_GZOpen, pr2) && pr2[0];
    uint32_t pr3[8] = { pr2[0], ros_addr(out), 100 }, pe[8] = { pr2[0] }, pc2[8] = { pr2[0] };
    gzo = gzo && !swi(XZLib_GZRead, pr3) && pr3[0] == 30 && !memcmp(out, back, 30) &&
          !swi(XZLib_GZError, pe) && pe[1] == 0 && !swi(XZLib_GZClose, pc2);
    /* truncated: what there is, then the error and its message */
    uint32_t tn = str("HostFS::ZTest.$.GZShort");
    memcpy(out, farm_gz, 60);
    uint32_t sv[8] = { 10, tn, 0xFFD, 0, oa, oa + 60 };
    uint32_t to[8] = { tn, str("rb") };
    gzo = gzo && !swi(XOS_File, sv) && !swi(XZLib_GZOpen, to) && to[0];
    uint32_t tr[8] = { to[0], ros_addr(back) + 1000, 1000 }, te[8] = { to[0] }, tc[8] = { to[0] };
    gzo = gzo && !swi(XZLib_GZRead, tr) && (int32_t)tr[0] >= 0 && !swi(XZLib_GZError, te) &&
          te[1] == (uint32_t)-5 && !strcmp(ros_ptr(te[0]), "HostFS::ZTest.$.GZShort: unexpected end of file") &&
          !swi(XZLib_GZClose, tc) && tc[0] == (uint32_t)-5;
    /* a gzip file whose extra field is longer than the 1024 bytes 'R' keeps:
     * the parser stops at what was kept and finds no 'AC' field */
    {
        uint8_t *lf = (uint8_t *)ros_rma_alloc(2100);
        memset(lf, 0, 2100);
        static const uint8_t head[12] = { 0x1f, 0x8b, 8, 4, 0, 0, 0, 0, 2, 13, 0xD0, 0x07 };
        memcpy(lf, head, 12);
        for (int i = 0; i < 250; i++) {
            lf[12 + i * 8] = 'X', lf[13 + i * 8] = 'Y', lf[14 + i * 8] = 4;
        }
        uint32_t ln = str("HostFS::ZTest.$.GZLong");
        uint32_t lsv[8] = { 10, ln, 0xFFD, 0, ros_addr(lf), ros_addr(lf) + 2100 };
        uint32_t lo[8] = { ln, str("rbR"), 1, 2, 3, 4 };
        int lg = !swi(XOS_File, lsv) && !swi(XZLib_GZOpen, lo) && lo[0] != 0 && lo[2] == 1 &&
                 lo[3] == 2 && lo[4] == 3 && lo[5] == 4;
        uint32_t lc[8] = { lo[0] };
        if (lo[0])
            swi(XZLib_GZClose, lc);
        check(lg, "ZLib_GZOpen 'R': an extra field longer than the buffer is read as far as it was "
              "kept, and no 'AC' field is found in what was not", NULL);
    }
    scratch_gone("ZTest", dir);
    check(gzw && gzr && gzo, "ZLib's gzip files: 'wb9R' writes 5.30's 70 bytes (RISC OS's 'AC' "
          "header field, OS 13, a seek on writing zeros); 'rbR' reads the field back into R2-R5, "
          "reads, seeks both ways, EOF and tell as 5.30's; a missing file or a bad mode is 0; a "
          "plain file ('T') reads through; a short one ends Z_BUF_ERROR, \"unexpected end of file\"", NULL);
}
