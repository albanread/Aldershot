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
 * (Sources/Programmer/BASIC: s.Array, s.Factor).
 */

#ifndef FACTOR_RESID_H
#define FACTOR_RESID_H

#include <stdint.h>

struct ros_cpu;

/* FACTOR's remaining one-off built-ins.  This is the final writer
 * unit.  It covers every dispatch case that row 5's hand dispatcher
 * still left to the renamed lift trunk.  They are all hand code now, in
 * the rig's one-argument case shape: R[14] is the live return address,
 * the state is in R[], and the arena stack is kept word for word.
 *
 * Factor.s:
 *   - TWOFUNC and TWOFUNCA (the two escape trunks, 293-321) and the
 *     bodies in their tables, SUM (Array.s:1543) and BEAT
 *     (Factor.s:392)
 *   - TWOFUNCA's RQUIT RSYS RTINT RBEATS RTEMPO
 *   - DIMFN (324-357), REPFN (the REP$ reader), WIDTHFN TRACEFN MODEFN
 *     (371-377) and VDUFN (379)
 *   - ADC BBGET COUNT GIVEEND ERL ERR EXT (the OS and state readers)
 *   - FALSE TRUE GET INKEY NOT OPENU OPENI OPENO POINTB POS RND TO USR
 *     VPOS GETD INKED STRD EOF
 * Array.s:
 *   - SUM/SUMLEN (1604) and MODULUS (1615).
 *
 * With this unit the hand dispatcher's default arm is FACERR, which is
 * the lift's own arm for every error character.  So the swap no longer
 * tail-calls basicvfp_FACTOR_lift from any path, and row 8's scaffold
 * drop deletes the whole lift trunk. */
void basicvfp_hand_TWOFUNC(struct ros_cpu *s);
void basicvfp_hand_TWOFUNCA(struct ros_cpu *s);
void basicvfp_hand_DIMFN(struct ros_cpu *s);
void basicvfp_hand_REPFN(struct ros_cpu *s);
void basicvfp_hand_WIDTHFN(struct ros_cpu *s);
void basicvfp_hand_TRACEFN(struct ros_cpu *s);
void basicvfp_hand_MODEFN(struct ros_cpu *s);
void basicvfp_hand_VDUFN(struct ros_cpu *s);
void basicvfp_hand_BEAT(struct ros_cpu *s);
void basicvfp_hand_RBEATS(struct ros_cpu *s);
void basicvfp_hand_RTEMPO(struct ros_cpu *s);
void basicvfp_hand_ADC(struct ros_cpu *s);
void basicvfp_hand_TRUE(struct ros_cpu *s);
void basicvfp_hand_BBGET(struct ros_cpu *s);
void basicvfp_hand_COUNT(struct ros_cpu *s);
void basicvfp_hand_GIVEEND(struct ros_cpu *s);
void basicvfp_hand_ERL(struct ros_cpu *s);
void basicvfp_hand_ERR(struct ros_cpu *s);
void basicvfp_hand_EXT(struct ros_cpu *s);
void basicvfp_hand_RSYS(struct ros_cpu *s);
void basicvfp_hand_RQUIT(struct ros_cpu *s);
void basicvfp_hand_FALSE(struct ros_cpu *s);
void basicvfp_hand_GET(struct ros_cpu *s);
void basicvfp_hand_INKEY(struct ros_cpu *s);
void basicvfp_hand_NOT(struct ros_cpu *s);
void basicvfp_hand_OPENU(struct ros_cpu *s);
void basicvfp_hand_OPENI(struct ros_cpu *s);
void basicvfp_hand_OPENO(struct ros_cpu *s);
void basicvfp_hand_POINTB(struct ros_cpu *s);
void basicvfp_hand_RTINT(struct ros_cpu *s);
void basicvfp_hand_POS(struct ros_cpu *s);
void basicvfp_hand_RND(struct ros_cpu *s);
void basicvfp_hand_TO(struct ros_cpu *s);
void basicvfp_hand_USR(struct ros_cpu *s);
void basicvfp_hand_VPOS(struct ros_cpu *s);
void basicvfp_hand_GETD(struct ros_cpu *s);
void basicvfp_hand_INKED(struct ros_cpu *s);
void basicvfp_hand_STRD(struct ros_cpu *s);
void basicvfp_hand_EOF(struct ros_cpu *s);
void basicvfp_hand_SUM(struct ros_cpu *s);
void basicvfp_hand_MODULUS(struct ros_cpu *s);

#endif
