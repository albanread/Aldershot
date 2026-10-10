/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file is a reimplementation of parts of RISC OS Open's Kernel
 * (Sources/Kernel). The Apache licence of RISC OS Open's source applies to it.
 */

/* cmos.c: CMOS RAM, simulated in a file, behind OS_Byte 161 and 162 and
 * OS_NVMemory.
 *
 * The file is the team emulator's (RISCOSQEMUA72: mkcmos.py, and HostFS's
 * *SaveCMOS on every OS_Byte 162). It is CMOS,ff2 in the root of the HostFS
 * share. It holds 2048 bytes in logical order, so byte N is what OS_Byte 161
 * reads at N. After them comes the OS version, a little-endian word from 500
 * to 599. So a share moves between the emulator and the box with its
 * configuration.
 *
 * The rules are the kernel's (Kernel/s/PMF/i2cutils and osbyte).
 * - An address past the end reads 0 and is not written.
 * - The Econet station number (0) is not written (ProtectStationID).
 * - &F0 to &FF are the one-time-programmable bytes. They are read but not
 *   written.
 * - Every other write moves the checksum at &EF by the byte's change, unless
 *   the checksum itself is being written.
 * Each change is written to the file at once, as HostFS's hook saves on every
 * OS_Byte 162.
 *
 * The file is $ROSGD_CMOS. In the box it is the share's /host/CMOS,ff2, made
 * from the defaults when it is not there, as the emulator's launcher copies
 * cmos.bin in. Hosted, with no $ROSGD_CMOS, CMOS is held in memory alone.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rosgd/api.h"
#include "rosgd/arena.h"
#include "rosgd/cmos.h"
#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/swi.h"

#define CHECKSUM    0xEFu       /* CheckSumCMOS */
#define NET_STATION 0x00u       /* NetStnCMOS */
#define OTP_FROM    0xF0u
#define OTP_TO      0x100u
#define ERR_BAD_REASON        0x180u
#define ERR_CORE_NOT_READABLE 0x410u
#define ERR_CORE_NOT_WRITABLE 0x411u

static uint8_t cmos[ROS_CMOS_SIZE + 4];
static char file[1024];
static int attached, backed;

static void save(void)
{
    if (!backed)
        return;
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    ssize_t n = write(fd, cmos, sizeof cmos);
    (void)n;
    close(fd);
}

void ros_cmos_attach(const char *path)
{
    attached = 1;
    memcpy(cmos, ros_cmos_default, sizeof cmos);
    backed = path != NULL;
    file[0] = 0;
    if (!path)
        return;
    snprintf(file, sizeof file, "%s", path);
    uint8_t in[ROS_CMOS_SIZE + 4];
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, in, sizeof in) : -1;
    if (fd >= 0)
        close(fd);
    if (n < 0) {                                    /* none: the defaults, kept */
        save();
        return;
    }
    uint32_t version = (uint32_t)in[ROS_CMOS_SIZE] | (uint32_t)in[ROS_CMOS_SIZE + 1] << 8 |
                       (uint32_t)in[ROS_CMOS_SIZE + 2] << 16 | (uint32_t)in[ROS_CMOS_SIZE + 3] << 24;
    if (n == (ssize_t)sizeof in && version >= 500 && version < 600)
        memcpy(cmos, in, sizeof in);
    /* Otherwise the file is not used, as the HAL does with a bad version.
     * The defaults stand and replace the file at the next change. */
}

static void attach_default(void)
{
    const char *env = getenv("ROSGD_CMOS");
    struct stat st;
    if (env && *env)
        ros_cmos_attach(env);
    else if (getpid() == 1 && stat("/host", &st) == 0 && S_ISDIR(st.st_mode))
        ros_cmos_attach("/host/CMOS,ff2");
    else
        ros_cmos_attach(NULL);
}

const char *ros_cmos_path(void)
{
    if (!attached)
        attach_default();
    return backed ? file : NULL;
}

uint8_t ros_cmos_read(uint32_t address)
{
    if (!attached)
        attach_default();
    return address < ROS_CMOS_SIZE ? cmos[address] : 0;
}

/* The kernel's Write, without its station-number test. */
static int put(uint32_t address, uint8_t value)
{
    if (!attached)
        attach_default();
    if (address >= ROS_CMOS_SIZE)
        return -1;
    if (address >= OTP_FROM && address < OTP_TO)
        return 0;
    uint8_t old = cmos[address];
    if (old == value)
        return 0;
    if (address != CHECKSUM)
        cmos[CHECKSUM] = (uint8_t)(cmos[CHECKSUM] - old + value);
    cmos[address] = value;
    save();
    return 0;
}

int ros_cmos_write(uint32_t address, uint8_t value)
{
    if (address == NET_STATION)
        return 0;
    return put(address, value);
}

/* ---- OS_Byte 161 and 162 (osbyte.c), OS_NVMemory ---- */

void ros_cmos_byte(struct ros_cpu *s)
{
    if (s->r[0] == 161) {
        s->r[2] = ros_cmos_read(s->r[1]);
    } else {
        ros_cmos_write(s->r[1], (uint8_t)s->r[2]);
    }
}

void ros_thunk_OS_NVMemory(struct ros_cpu *s)
{
    s->v = 0;
    switch (s->r[0]) {
    case 0:                                         /* the size */
        s->r[1] = ROS_CMOS_SIZE;
        return;
    case 1:                                         /* a byte */
        if (s->r[1] >= ROS_CMOS_SIZE)
            break;
        s->r[2] = ros_cmos_read(s->r[1]);
        return;
    case 2:
        if (s->r[1] >= ROS_CMOS_SIZE && s->r[1] != NET_STATION) {
            ros_swi_fail(s, ros_error(ERR_CORE_NOT_WRITABLE, "No writable memory at this address"));
            return;
        }
        ros_cmos_write(s->r[1], (uint8_t)s->r[2]);
        return;
    case 3:                                         /* a block: R1 location, R2 buffer, R3 length */
        for (uint32_t i = 0; i < s->r[3]; i++)
            ros_st8(s->r[2] + i, ros_cmos_read(s->r[1] + i));
        return;
    case 4:
        for (uint32_t i = 0; i < s->r[3]; i++)
            if (s->r[1] + i != NET_STATION)
                put(s->r[1] + i, ros_ld8(s->r[2] + i));
        return;
    default:
        ros_swi_fail(s, ros_error(ERR_BAD_REASON, "Bad reason code"));
        return;
    }
    ros_swi_fail(s, ros_error(ERR_CORE_NOT_READABLE, "No readable memory at this address"));
}
