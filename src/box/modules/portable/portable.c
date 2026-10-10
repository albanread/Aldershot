/* Copyright 2010 Castle Technology Ltd
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
 * This file is a reimplementation in C of RISC OS Open's Portable module
 * (Sources/HWSupport/PortableHAL: s.StPortable).
 */

/* portable.c: Portable (HWSupport/PortableHAL, 0.81), reimplemented. It is
 * the module that the Pi's ROM has, now on the box. The box has no CPU
 * clock device and no battery management unit (BMU).
 *
 * The important call is Portable_Idle. The Wimp counts the idle polls in a
 * second with a CallEvery. When it finds itself idle for a while, it goes
 * into power saving. It then calls Portable_Idle on each poll that finds
 * nothing to do. On the Pi this is a WFI, which stops the processor until
 * the next interrupt (the keyboard, the mouse or the centisecond timer).
 * Here it is a wait with the personality lock released. The wait ends when
 * background work is queued (input, or the ticker's centisecond) or after
 * ten milliseconds, whichever is first. The queued work is then run
 * (ros_idle). This lets input into an idle desktop, whose Wimp would
 * otherwise poll inside its SWI for ever. It also stops an idle box from
 * spinning a host processor.
 *
 * The other calls do what s/StPortable does when there are no devices.
 * Portable_ReadFeatures reports Idle alone (bit 4). Bit 0, Speed, needs a
 * CPU clock device. Portable_Speed keeps the setting, (old AND R1) EOR R0,
 * and has no clock to change. Portable_Speed2 gives "unsupported on this
 * hardware". The BMU calls find no BMU, and ReadSensor finds no sensor.
 * The other calls give the kernel's "SWI value out of range". The error
 * numbers are the module's (ErrorBase_Portable, &B40) and the texts are
 * the UK texts. */
#include <stdint.h>

#include "portable.h"
#include "rosgd/api.h"
#include "rosgd/background.h"
#include "rosgd/cpu.h"
#include "rosgd/swi.h"

#define FEATURE_IDLE 0x10u              /* PortableFeature_Idle */

/* ErrorBase_Portable (hdr/NewErrors), in s/StPortable's order */
enum {
    E_BAD_BMU_VARIABLE = 0xB40, E_BAD_BMU_COMMAND, E_BMU_BUSY, E_BAD_BMU_VERSION, E_BMU_FAULT,
    E_BMU_VEC_CLAIM, E_CANT_FREEZE, E_FREEZE_FAILED, E_NO_SPEED2, E_BAD_SPEED2, E_BAD_FLAGS,
    E_BAD_BMU, E_UK_SENSOR
};

static uint32_t cpu_speed;              /* CPUSpeed: 0 fast, 1 slow */

static void fail(struct ros_cpu *s, uint32_t n, const char *text)
{
    ros_swi_fail(s, ros_error(n, "%s", text));
}

static void bad_swi_error(struct ros_cpu *s)
{
    ros_swi_fail(s, ros_error(0x1E6, "SWI value out of range for module Portable"));
}

/* Portable_Speed: new = (old AND R1) EOR R0; R0 the old, R1 the new */
void ros_thunk_Portable_Speed(struct ros_cpu *s)
{
    uint32_t old = cpu_speed, now = (old & s->r[1]) ^ s->r[0];
    cpu_speed = now;
    s->r[0] = old, s->r[1] = now;
    s->v = 0;
}

void ros_thunk_Portable_ReadFeatures(struct ros_cpu *s)
{
    s->r[1] = FEATURE_IDLE;
    s->v = 0;
}

/* Portable_Idle: waits until the next "interrupt". That is queued
 * background work, or the centisecond. */
void ros_thunk_Portable_Idle(struct ros_cpu *s)
{
    ros_idle(10);
    s->v = 0;
}

void ros_thunk_Portable_Speed2(struct ros_cpu *s)
{
    fail(s, E_NO_SPEED2, "Portable_Speed2 is unsupported on this hardware");
}

/* There is no BMU. EnumerateBMU's flags must be 0, and then there is none to list. */
void ros_thunk_Portable_EnumerateBMU(struct ros_cpu *s)
{
    if (s->r[1])
        fail(s, E_BAD_FLAGS, "Bad flags");
    else
        fail(s, E_BAD_BMU, "Invalid BMU index");
}

void ros_thunk_Portable_ReadBMUVariables(struct ros_cpu *s)
{
    fail(s, E_BAD_BMU, "Invalid BMU index");
}

void ros_thunk_Portable_ReadBMUVariable(struct ros_cpu *s)
{
    fail(s, E_BAD_BMU, "Invalid BMU index");
}

/* There is no sensor. The die temperature needs a CPU clock device and a BMU's sensor needs a BMU. */
void ros_thunk_Portable_ReadSensor(struct ros_cpu *s)
{
    fail(s, E_UK_SENSOR, "Unrecognised sensor index");
}

#define BAD(n) \
    void ros_thunk_Portable_##n(struct ros_cpu *s) { bad_swi_error(s); }
BAD(Control)
BAD(WriteBMUVariable)
BAD(CommandBMU)
BAD(Stop)
BAD(Status)
BAD(Contrast)
BAD(Refresh)
BAD(Halt)
BAD(SleepTime)
BAD(SMBusOp)
BAD(WakeTime)

static os_error *bad_swi(struct ros_module *m, uint32_t offset)
{
    (void)m, (void)offset;
    return ros_error(0x1E6, "SWI value out of range for module Portable");
}

struct ros_module portable_module = {
    .title = "Portable",
    .help = "Portable\t\t0.81 (20 Jan 2017) ROSGD native",
    .bad_swi = bad_swi,
    .swi_chunk = 0x42FC0,
    .swi_thunks = ros_swi_thunks_Portable,
    .swi_names = ros_swi_names_Portable,
    .swi_prefix = "Portable",
};

__attribute__((constructor)) static void count(void)
{
    portable_module.swi_count = ros_swi_count_Portable;
}
