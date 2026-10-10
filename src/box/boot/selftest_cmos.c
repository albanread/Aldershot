/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_cmos.c: CMOS, simulated in a file (runtime/cmos.c), through
 * OS_Byte 161 and 162 and OS_NVMemory, against the kernel's rules and the
 * team emulator's file.
 *
 * The test attaches files of its own in TMPDIR (or the box's /tmp), and puts
 * back the CMOS it found at the end.  In the box that is the share's
 * CMOS,ff2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cmos.h"
#include "rosgd/cpu.h"
#include "rosgd/rma.h"
#include "rosgd/swi.h"
#include "selftest.h"

#define check ros_check

static uint32_t byte(uint32_t a, uint32_t x, uint32_t y)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = a, s.r[1] = x, s.r[2] = y;
    ros_swi(&s, XOS_Byte);
    return s.r[2];
}

static int nvmemory(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t out[4])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = r0, s.r[1] = r1, s.r[2] = r2, s.r[3] = r3;
    ros_swi(&s, XOS_NVMemory);
    memcpy(out, s.r, 4 * sizeof out[0]);
    return s.v ? (int)((os_error *)ros_ptr(s.r[0]))->errnum : 0;
}

/* The kernel's checksum: 1 + logical 0-&EE and &100-&7FF */
static int checksum_ok(const uint8_t *c)
{
    unsigned sum = 1;
    for (unsigned i = 0; i < ROS_CMOS_SIZE; i++)
        if (i < 0xEF || i >= 0x100)
            sum += c[i];
    return (sum & 0xFF) == c[0xEF];
}

static int read_file(const char *path, uint8_t *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    size_t got = f ? fread(buf, 1, n + 1, f) : 0;
    if (f)
        fclose(f);
    return f && got == n;
}

static void write_file(const char *path, const uint8_t *buf, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, n, f);
        fclose(f);
    }
}

void ros_selftest_cmos(void)
{
    char before[1024] = "";
    const char *was = ros_cmos_path();
    if (was)
        snprintf(before, sizeof before, "%s", was);

    const char *tmp = getenv("TMPDIR");
    struct stat hs;
    if (!(tmp && *tmp))
        tmp = stat("/host", &hs) == 0 && S_ISDIR(hs.st_mode) ? "/host" : "/tmp";
    mkdir(tmp, 0777);
    char path[600];
    snprintf(path, sizeof path, "%s/rosgd-cmos-%d", tmp, (int)getpid());
    unlink(path);

    /* Memory alone: the defaults */
    ros_cmos_attach(NULL);
    uint8_t c[ROS_CMOS_SIZE + 4];
    for (unsigned i = 0; i < ROS_CMOS_SIZE; i++)
        c[i] = (uint8_t)byte(161, i, 0);
    check(c[5] == 220 && checksum_ok(c) && byte(161, 5000, 0x55) == 0,
          "CMOS -- the emulator's defaults: FileSystem HostFS (220), a good checksum; past the end reads 0",
          "FileSystem %u, checksum %s", c[5], checksum_ok(c) ? "good" : "bad");

    /* A file not there: made from the defaults */
    ros_cmos_attach(path);
    int made = read_file(path, c, sizeof c) && !memcmp(c, ros_cmos_default, sizeof c);
    check(made, "CMOS -- a file not there is made from the defaults, 2052 bytes", "%s", path);

    /* OS_Byte 162: written through, the checksum kept; the station number
     * and the OTP bytes left alone */
    uint8_t old = (uint8_t)byte(161, 0x10, 0);
    byte(162, 0x10, (uint8_t)(old ^ 0x5A));
    byte(162, 0x00, 0x77);
    byte(162, 0xF5, 0x12);
    int wrote = read_file(path, c, sizeof c) && c[0x10] == (uint8_t)(old ^ 0x5A) && checksum_ok(c) &&
                c[0] == ros_cmos_default[0] && c[0xF5] == ros_cmos_default[0xF5] &&
                byte(161, 0x10, 0) == (uint8_t)(old ^ 0x5A);
    check(wrote, "CMOS -- OS_Byte 162 rewrites the file, keeps the checksum; station and OTP bytes not written",
          "&10 = &%02X, checksum %s", c[0x10], checksum_ok(c) ? "good" : "bad");

    /* OS_NVMemory */
    uint32_t r[4];
    uint8_t *blk = ros_rma_alloc(16);
    int nv = XOS_NVMemory == 0x20076 && !nvmemory(0, 0, 0, 0, r) && r[1] == ROS_CMOS_SIZE && !nvmemory(1, 5, 0, 0, r) && r[2] == 220 &&
             !nvmemory(3, 0x10, ros_addr(blk), 4, r) && blk[0] == (uint8_t)(old ^ 0x5A) &&
             !nvmemory(2, 0x10, old, 0, r) && byte(161, 0x10, 0) == old &&
             nvmemory(1, 3000, 0, 0, r) == 0x410 && nvmemory(2, 3000, 1, 0, r) == 0x411 &&
             nvmemory(9, 0, 0, 0, r) == 0x180;
    ros_rma_free(blk);
    check(nv, "CMOS -- OS_NVMemory (&76, as Kernel/hdr/RISCOS numbers it) 0-4, and the kernel's errors past the end and for a bad reason", " ");

    /* Loaded again: what was written stays */
    byte(162, 0x11, 0xA5);
    ros_cmos_attach(path);
    check(byte(161, 0x11, 0) == 0xA5, "CMOS -- attached again, the file's contents are what OS_Byte 161 reads",
          "&11 = &%02X", byte(161, 0x11, 0));

    /* A file of another OS's CMOS (the version out of 500-599): not used */
    memcpy(c, ros_cmos_default, sizeof c);
    c[0x11] = 0x3C;
    c[ROS_CMOS_SIZE] = 0x10, c[ROS_CMOS_SIZE + 1] = 0x01;          /* 272 */
    write_file(path, c, sizeof c);
    ros_cmos_attach(path);
    check(byte(161, 0x11, 0) == ros_cmos_default[0x11], "CMOS -- a file with a version out of range is not used",
          " ");

    /* *ReadCMOSIP: bytes 108, 109, 110 and 0 */
    memcpy(c, ros_cmos_default, sizeof c);
    c[108] = 192, c[109] = 168, c[110] = 1, c[0] = 42;
    write_file(path, c, sizeof c);
    ros_cmos_attach(path);
    char *cmd = ros_rma_alloc(64), *val = ros_rma_alloc(64);
    strcpy(cmd, "ReadCMOSIP");
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(cmd);
    ros_swi(&s, XOS_CLI);
    int cli_ok = !s.v;
    strcpy(cmd, "Inet$CMOSIPAddr");
    ros_cpu_enter(&s);
    s.r[0] = ros_addr(cmd), s.r[1] = ros_addr(val), s.r[2] = 63, s.r[3] = 0, s.r[4] = 3;
    ros_swi(&s, XOS_ReadVarVal);
    val[s.v ? 0 : s.r[2]] = 0;
    check(cli_ok && !strcmp(val, "192.168.1.42"), "CMOS -- *ReadCMOSIP reads the address kept in CMOS",
          "\"%s\"", val);
    ros_rma_free(cmd), ros_rma_free(val);

    unlink(path);
    ros_cmos_attach(before[0] ? before : NULL);     /* the CMOS it found */
}
