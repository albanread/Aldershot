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
 * (Sources/Programmer/BASIC: s.Basic).
 */

/* dispat_swap.h -- the hand statement dispatcher. This is the scaffold
 * that replaces basicvfp_DISPAT as a whole (units/dispat_swap.c).
 *
 * The lift's DISPAT is one function of 11,826 lines. A prologue loads
 * its locals from R[]. Then a switch on the token sends its 173 cases
 * by goto to the 85 statement bodies, which are internal labels. None
 * of the bodies can be given a thunk of its own, so the swap replaces
 * the DISPATCHER and leaves the bodies alone.
 *
 * This unit's basicvfp_hand_DISPAT reads the token. On entry the lift's
 * contract is: R10 the current token, R12 LINE just past it, R8 ARGP,
 * R13 SP, and the other registers whatever the previous statement left.
 * If the token's statement has a hand function, the dispatcher calls it
 * with ROS_TAIL_CALL. This is the same guaranteed tail call that every
 * CASE_SUBS arm uses, so no dispatcher frame survives into the
 * statement. A token WITHOUT hand coverage tail-calls the lift's own
 * dispatcher, which substitute.py has renamed basicvfp_DISPAT_lift with
 * its body unchanged. So the default path IS the lift's dispatch. The
 * swap passes the gates even with no statements covered, and statements
 * can be moved over one arm at a time.
 *
 * The mechanism is opt-in and is OFF by default: manifest.DISPAT_SWAP
 * ('ENABLE': False). When it is enabled, substitute.py does three
 * things:
 *  - it renames the lift's dispatcher;
 *  - it puts a musttail thunk basicvfp_DISPAT -> basicvfp_hand_DISPAT
 *    in its place, so the code-table registration at &FC100BEC and
 *    every caller (hand STMT, and the lift's DC immediate-mode entry)
 *    reach the hand dispatcher;
 *  - it writes gen/dispat_swap_cfg.h, the compile-time wiring that this
 *    unit includes. Each group's arms compile only when that group's
 *    unit file is present, so the dispatcher always covers exactly what
 *    has landed.
 */

#ifndef DISPAT_SWAP_H
#define DISPAT_SWAP_H

struct ros_cpu;

void basicvfp_hand_DISPAT(struct ros_cpu *s);

#endif
