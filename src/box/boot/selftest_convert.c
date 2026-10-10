/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_convert.c: the conversions, against the kernel's own
 * (Kernel/s/Convrsions, ArthurSWIs).  They are called through the SWIs, as
 * programs call them.  The expected strings are worked out from that source.
 */
#include <string.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "selftest.h"

#define check ros_check

static uint32_t buf, in;        /* the output buffer, an input block */

static int swi(uint32_t n, uint32_t r[5])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 5 * sizeof r[0]);
    ros_swi(&s, n);
    memcpy(r, s.r, 5 * sizeof r[0]);
    return s.v;
}

static uint32_t errnum(const uint32_t r[5])
{
    return ((os_error *)ros_ptr(r[0]))->errnum;
}

/* A table conversion of v into a 32-byte buffer: the string, and the
 * registers as the contract has them. */
static void conv(uint32_t n, uint32_t v, const char *want)
{
    uint32_t r[5] = { v, buf, 32, 0, 0 };
    int bad = swi(n, r);
    size_t len = strlen(want);
    int ok = !bad && r[0] == buf && r[1] == buf + len && r[2] == 32 - len &&
             strcmp(ros_ptr(buf), want) == 0;
    check(ok, "OS_Convert -- a value to a string", "SWI &%X of &%X: \"%s\" R1 +%d R2 %u, want \"%s\"",
          n, v, bad ? "(error)" : (const char *)ros_ptr(buf), (int)(r[1] - buf), r[2], want);
}

static void variform(uint32_t type, const uint8_t *bytes, uint32_t n, uint32_t size,
                     const char *want)
{
    memcpy(ros_ptr(in), bytes, size);
    uint32_t r[5] = { in, buf, 64, n, type };
    int bad = swi(XOS_ConvertVariform, r);
    check(!bad && r[1] == buf + strlen(want) && strcmp(ros_ptr(buf), want) == 0,
          "OS_ConvertVariform -- a type to a string", "type %u: \"%s\", want \"%s\"", type,
          bad ? "(error)" : (const char *)ros_ptr(buf), want);
}

/* OS_ReadUnsigned of text: V, and R1's offset, R2, R3 after. */
static int read_unsigned(const char *text, uint32_t flags, uint32_t r[5])
{
    strcpy(ros_ptr(in), text);
    uint32_t limit = r[2], hi = r[3], wide = r[4];
    r[0] = flags, r[1] = in, r[2] = limit, r[3] = hi, r[4] = wide;
    int bad = swi(XOS_ReadUnsigned, r);
    r[1] -= in;
    return bad;
}

void ros_selftest_convert(void)
{
    buf = ros_addr(ros_rma_alloc(64));
    in = ros_addr(ros_rma_alloc(64));

    /* ---- the table-driven ones ---- */
    conv(XOS_ConvertHex1, 0xAB, "B");
    conv(XOS_ConvertHex2, 0x1FF, "FF");
    conv(XOS_ConvertHex4, 0x1234ABCD, "ABCD");
    conv(XOS_ConvertHex6, 0x12345678, "345678");
    conv(XOS_ConvertHex8, 0x1234ABCD, "1234ABCD");
    conv(XOS_ConvertCardinal1, 0, "0");
    conv(XOS_ConvertCardinal2, 0x10000, "0");
    conv(XOS_ConvertCardinal4, 0xFFFFFFFF, "4294967295");
    conv(XOS_ConvertInteger1, 0xFF, "-1");
    conv(XOS_ConvertInteger2, 0x7FFF, "32767");
    conv(XOS_ConvertInteger3, 0x800000, "-8388608");
    conv(XOS_ConvertInteger4, 0x80000000, "-2147483648");
    conv(XOS_ConvertBinary1, 5, "00000101");
    conv(XOS_ConvertBinary2, 0x8001, "1000000000000001");
    conv(XOS_ConvertSpacedCardinal4, 1234567, "1 234 567");
    conv(XOS_ConvertSpacedCardinal4, 1000, "1 000");
    conv(XOS_ConvertSpacedCardinal2, 999, "999");
    conv(XOS_ConvertSpacedInteger4, (uint32_t)-1234, "-1 234");

    /* ---- file sizes: to four digits and a unit, rounded ---- */
    conv(XOS_ConvertFileSize, 0, "0 bytes");
    conv(XOS_ConvertFileSize, 1, "1 byte");
    conv(XOS_ConvertFileSize, 4095, "4095 bytes");
    conv(XOS_ConvertFileSize, 4096, "4 kbytes");
    conv(XOS_ConvertFileSize, 1536 * 1024, "1536 kbytes");
    conv(XOS_ConvertFileSize, 5 * 1024 * 1024, "5 Mbytes");
    conv(XOS_ConvertFileSize, 0xFFFFFFFF, "4 Gbytes");
    conv(XOS_ConvertFixedFileSize, 1, "   1  byte ");
    conv(XOS_ConvertFixedFileSize, 2000, "2000  bytes");
    conv(XOS_ConvertFixedFileSize, 4096, "   4 kbytes");

    /* ---- Econet net.station ---- */
    ros_st32(in, 254), ros_st32(in + 4, 0);
    conv(XOS_ConvertFixedNetStation, in, "    254");
    conv(XOS_ConvertNetStation, in, "254");
    ros_st32(in, 5), ros_st32(in + 4, 1);
    conv(XOS_ConvertFixedNetStation, in, "  1.005");
    conv(XOS_ConvertNetStation, in, "1.5");
    uint32_t r[5] = { in, buf, 32, 0, 0 };
    ros_st32(in, 5), ros_st32(in + 4, 256);
    int v1 = swi(XOS_ConvertNetStation, r);
    uint32_t e1 = errnum(r);
    r[0] = in, r[1] = buf, r[2] = 32;
    ros_st32(in, 0), ros_st32(in + 4, 1);
    int v2 = swi(XOS_ConvertNetStation, r);
    check(v1 && e1 == 0x307 && v2 && errnum(r) == 0x306,
          "OS_ConvertNetStation -- \"Bad network number\" &307, \"Bad station number\" &306", NULL);

    /* ---- the buffer: the terminator needs a byte but does not use it ---- */
    r[0] = 0x1234ABCD, r[1] = buf, r[2] = 8;
    int v = swi(XOS_ConvertHex8, r);
    uint32_t e = v ? errnum(r) : 0;
    r[0] = 0x1234ABCD, r[1] = buf, r[2] = 9;
    int ok9 = !swi(XOS_ConvertHex8, r) && r[2] == 1 && r[1] == buf + 8;
    check(v && e == 0x1E4 && ok9, "OS_ConvertHex8 -- 8 bytes overflow, &1E4; 9 leave R2 = 1",
          NULL);

    /* ---- OS_ConvertVariform: the types the table does not reach ---- */
    const uint8_t n1234567[4] = { 0x87, 0xD6, 0x12, 0x00 };
    variform(6, n1234567, 4, 4, "1,234,567");
    variform(7, (const uint8_t[]){ 0x2E, 0xFB, 0xFF, 0xFF }, 4, 4, "-1,234");
    variform(10, (const uint8_t[]){ 1, 2, 0, 10 }, 4, 4, "10.0.2.1");
    variform(11, (const uint8_t[]){ 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 }, 6, 6,
             "11:22:33:44:55:66");
    uint8_t v6[16] = { 1 };
    variform(12, v6, 16, 16, "0:0:0:0:0:0:0:1");
    variform(13, v6, 16, 16, "::1");
    memset(v6, 0, sizeof v6);
    v6[14] = 1;
    variform(13, v6, 16, 16, "1::");
    /* 2001:db8:0:1:1:1:1:1, from the top halfword down: a run of one stays */
    const uint8_t v6b[16] = { 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0xB8, 0x0D, 0x01, 0x20 };
    variform(13, v6b, 16, 16, "2001:db8:0:1:1:1:1:1");
    uint8_t id[16];
    for (int i = 0; i < 16; i++)
        id[i] = (uint8_t)i;
    variform(15, id, 16, 16, "00010203-0405-0607-0809-0a0b0c0d0e0f");
    variform(14, (const uint8_t[]){ 0xCD, 0xAB }, 3, 2, "bcd");
    variform(1, n1234567, 0, 4, "");

    memcpy(ros_ptr(in), n1234567, 4);
    uint32_t c[5] = { in, buf, (uint32_t)-1, 4, 1 };
    v = swi(XOS_ConvertVariform, c);
    uint32_t need = ~c[2];
    e = v ? errnum(c) : 0;
    uint32_t h[5] = { in, buf, (uint32_t)-1, 8, 0 };
    swi(XOS_ConvertVariform, h);
    check(v && e == 0x1E4 && need == 7 && ~h[2] == 16,
          "OS_ConvertVariform, R2 < 0 -- the overflow, and R2 = NOT the length (hex: 2 a nybble, "
          "as the kernel)", "%u, hex %u", need, ~h[2]);
    c[0] = in, c[1] = buf, c[2] = 32, c[3] = 4, c[4] = 99;
    check(swi(XOS_ConvertVariform, c) && errnum(c) == 0x1F1,
          "OS_ConvertVariform -- a type it does not know: \"Unsupported conversion\" &1F1", NULL);

    /* ---- OS_BinaryToDecimal: no terminator, R2 the count ---- */
    uint32_t b[5] = { (uint32_t)-123, buf, 32, 0, 0 };
    int bv = swi(XOS_BinaryToDecimal, b);
    int b1 = !bv && b[2] == 4 && memcmp(ros_ptr(buf), "-123", 4) == 0;
    b[0] = 0x80000000, b[1] = buf, b[2] = 32;
    bv = swi(XOS_BinaryToDecimal, b);
    int b2 = !bv && b[2] == 11 && memcmp(ros_ptr(buf), "-2147483648", 11) == 0;
    b[0] = 0, b[1] = buf, b[2] = 32;
    bv = swi(XOS_BinaryToDecimal, b);
    int b3 = !bv && b[2] == 1 && ros_ld8(buf) == '0';
    b[0] = 123, b[1] = buf, b[2] = 2;
    int b4 = swi(XOS_BinaryToDecimal, b) && errnum(b) == 0x1E4;
    b[0] = 5, b[1] = buf, b[2] = (uint32_t)-1;
    int b5 = swi(XOS_BinaryToDecimal, b) && errnum(b) == 0x1E4 && b[2] == 0;
    check(b1 && b2 && b3 && b4 && b5,
          "OS_BinaryToDecimal -- -123, minint, 0; 123 into 2 bytes, or any into R2 < 0, "
          "overflows", NULL);

    /* ---- OS_ReadUnsigned ---- */
    uint32_t u[5] = { 0 };
    v = read_unsigned("  123 ", 10, u);
    check(!v && u[1] == 5 && u[2] == 123, "OS_ReadUnsigned -- spaces skipped, R1 after the digits",
          "%u at +%u", u[2], u[1]);
    v = read_unsigned("&1F", 10, u);
    int ok1 = !v && u[2] == 31;
    v = read_unsigned("2_101", 10, u);
    int ok2 = !v && u[2] == 5;
    v = read_unsigned("16_ff", 10, u);
    int ok3 = !v && u[2] == 255 && u[1] == 5;
    v = read_unsigned("ff", 16, u);
    int ok4 = !v && u[2] == 255;
    check(ok1 && ok2 && ok3 && ok4, "OS_ReadUnsigned -- &hex, base_number, and R0's base", NULL);

    u[2] = 0;
    v = read_unsigned("37_1", 10, u);
    int bad1 = v && errnum(u) == 0x16A && u[2] == 0 && u[1] == 0;
    v = read_unsigned("xyz", 10, u);
    int bad2 = v && errnum(u) == 0x16B && u[2] == 0 && u[1] == 0;
    v = read_unsigned("4294967296", 10, u);
    int bad3 = v && errnum(u) == 0x16C;
    check(bad1 && bad2 && bad3,
          "OS_ReadUnsigned -- \"Bad base\" &16A, \"Bad number\" &16B, \"Number too big\" &16C; "
          "R2 = 0, R1 unmoved", NULL);

    v = read_unsigned("12x", 0x8000000Au, u);
    int t1 = v && errnum(u) == 0x16B && u[1] == 2;
    v = read_unsigned("12\r", 0x8000000Au, u);
    int t2 = !v && u[2] == 12;
    v = read_unsigned("256", 0x4000000Au, u);
    int t3 = v && errnum(u) == 0x16C && u[1] == 3;
    u[2] = 100;
    v = read_unsigned("100", 0x2000000Au, u);
    int t4 = !v && u[2] == 100;
    u[2] = 100;
    v = read_unsigned("101", 0x2000000Au, u);
    int t5 = v && errnum(u) == 0x16C;
    check(t1 && t2 && t3 && t4 && t5,
          "OS_ReadUnsigned -- bit 31 the terminator, 30 to &FF, 29 to R2; R1 after the number",
          NULL);

    u[2] = u[3] = 0, u[4] = 0x45444957;
    v = read_unsigned("18446744073709551615", 0x1000000Au, u);
    check(!v && u[2] == 0xFFFFFFFF && u[3] == 0xFFFFFFFF && u[4] == 0xF0000000u,
          "OS_ReadUnsigned, R4 \"WIDE\" -- 64 bits into R2, R3; R4 the flags supported",
          "%08X %08X %08X", u[3], u[2], u[4]);
    u[4] = 0;

    ros_rma_free(ros_ptr(in));
    ros_rma_free(ros_ptr(buf));
}
