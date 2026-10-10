/* Copyright 1998 Acorn Computers Ltd
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
 * This file is a reimplementation in C of RISC OS Open's InetRes startup
 * utilities (Sources/SystemRes/InetRes/Sources/utils: s.CheckMem,
 * s.ReadCMOSIP, s.TriggerCBs).
 */

/* utils.c -- !Internet's Startup utilities (InetRes/Sources/utils, ObjAsm):
 * *CheckMem, *ReadCMOSIP, *TriggerCBs.
 *
 *   CheckMem    made sure the configured system heap was at least 8K and
 *               the RMA at least 256K, rewriting CMOS if not, before the
 *               stack started. ROSGD's heaps are the arena's, sized by the
 *               runtime and not by CMOS. So there is nothing to check, and
 *               it succeeds, so that !Internet's !Run carries on.
 *   ReadCMOSIP  the IP address in CMOS bytes 108, 109, 110 and 0, as
 *               Inet$CMOSIPAddr. 0.0.0.x is 10.0.0.x, and 0.0.0.0 sets
 *               nothing. It is read through OS_Byte 161, as the original
 *               does, from ROSGD's CMOS file (runtime/cmos.c).
 *   TriggerCBs  sixteen OS_Byte 0s, each a SWI's return, where callbacks
 *               run, so that work queued by the stack gets done.
 */
#include <stdio.h>
#include <string.h>

#include "inetres.h"
#include "rosgd/api.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"

static os_error *byte(uint32_t a, uint32_t x, uint32_t *y)
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    s.r[0] = a, s.r[1] = x, s.r[2] = 0;
    ros_swi(&s, ROS_X_BIT | 0x06);                  /* OS_Byte */
    if (s.v)
        return ros_ptr(s.r[0]);
    if (y)
        *y = s.r[2];
    return NULL;
}

os_error *inet_checkmem(const struct inet_args *a)
{
    (void)a;
    return NULL;
}

os_error *inet_readcmosip(const struct inet_args *a)
{
    (void)a;
    static const uint32_t where[4] = { 108, 109, 110, 0 };
    uint32_t ip[4];
    for (int i = 0; i < 4; i++) {
        os_error *e = byte(161, where[i], &ip[i]);  /* OS_Byte 161: read CMOS */
        if (e)
            return e;
    }
    if (!ip[0] && !ip[1] && !ip[2]) {
        if (!ip[3])
            return NULL;
        ip[0] = 10;
    }
    char text[20];
    snprintf(text, sizeof text, "%u.%u.%u.%u", ip[0] & 255, ip[1] & 255, ip[2] & 255, ip[3] & 255);
    inet_set_var("Inet$CMOSIPAddr", text);
    return NULL;
}

os_error *inet_triggercbs(const struct inet_args *a)
{
    (void)a;
    for (int i = 0; i < 16; i++) {
        os_error *e = byte(0, 1, NULL);             /* OS_Byte 0: the OS version */
        if (e)
            return e;
    }
    return NULL;
}
