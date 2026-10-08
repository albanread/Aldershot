/* Copyright 2001 Pace Micro Technology plc
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
 * This file is a translation into C of RISC OS Open's BBC BASIC source
 * (Sources/Programmer/BASIC: s.Basic, s.ModHead).
 */

/* modhead_shell.c -- the *BASICVFP command, translated by hand from
 * ModHead.s:223-230 (RISC OS 5.31's BASIC, the VFP build).  Unit 3's
 * first piece.
 *
 * The original is five instructions. It saves the return address and
 * hands the rest of the command line to OS_Module's Enter. Entering a
 * language does not return, so it ends by "returning" to the saved
 * address. In the lift that is a resume. The C stack unwinds to the
 * frame whose call made this one, and the module-enter glue (the
 * runtime's enter/exit catch) carries on from there.
 *
 * Basic_Services (ModHead.s:249) is left out on purpose. The lift
 * inlined Basic.s's MOVEMEMORY into it. That is the Service_Memory
 * handler, which relocates the stack, LOCALARLIST included, when the
 * Wimp moves the slot. It is a unit of its own, and the Wimp's own
 * tests are its gate. Everything else in ModHead is tables in the ROM
 * image, which tier 3 does not touch.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "modhead_shell.h"

#define ModHandReason_Enter 2

/* The SWI dispatcher's thunk, as gen/api_gen.h declares it. */
void ros_thunk_OS_Module(struct ros_cpu *s);

void basicvfp_hand_Basic_Code(struct ros_cpu *s)
{
    uint32_t r0 = s->r[0], r1, r2, r3 = s->r[14];
    r2 = r0;                                   /* the rest of the line */
    r0 = ModHandReason_Enter;
    r1 = 0xFC100089u;                          /* ADR R1,Basic_Title */
    s->r[0] = r0;
    s->r[1] = r1;
    s->r[2] = r2;
    s->r[3] = r3;
    ros_native_swi(s, ros_thunk_OS_Module);    /* SWI XOS_Module. In the
                                                 * X form an error comes
                                                 * back in V, which the
                                                 * original does not look
                                                 * at. */
    r3 = s->r[3];
    s->r[15] = r3;                             /* MOV PC,R3 */
    ros_resume(s, r3);
}
