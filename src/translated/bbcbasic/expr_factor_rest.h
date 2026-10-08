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
 * (Sources/Programmer/BASIC: s.Expr, s.Factor, s.Lexical).
 */

#ifndef EXPR_FACTOR_REST_H
#define EXPR_FACTOR_REST_H

#include <stdint.h>

struct ros_cpu;

/* The rest of unit 5: the expression evaluator's entry and trunk
 * machinery (Expr.s), and FACTOR's dispatch trunk with its core read
 * paths (Factor.s).  Each is a whole-function substitution unless
 * stated otherwise.
 *
 * Expr.s:
 *   - EXPR (785), EXPRRECUR (799), EXPRNEXT (808), EXPRHARD (813),
 *     AEEXPR (784)
 *   - the channel family AECHAN/CHAN/CHANNL (1336-1346)
 *   - ARLOOKCACHE with ARRAYREF/ARLOP/ARREND (505-567), which evaluate
 *     the index of every array element
 * Factor.s:
 *   - QSTR (434)
 *   - the typed variable read VARIND, with VARBYT/VARNOTNUM/VARSTR
 *     (472-525)
 *   - BRA (571), RPTR and its family (616-650), EVAL (719), VAL (1441)
 *     with VALSTR/VAL0, DATAST (419), RNUL/RNULX (1505-1506)
 * Lexical.s, the evaluator entry points that the statement bodies
 * share:
 *   - INTEXA/INTEXC (45-50), EQAEEX/AEEXDN (53-58), EXPRDN (59).
 *
 * FACTOR itself is a SWAP and not a substitution.  A thunk under the
 * old name reaches basicvfp_hand_FACTOR.  The hand dispatcher owns the
 * character switch for every case the interpreter can reach.  These are
 * the constants, the built-ins, the transcendentals, this unit's trunk
 * bodies, and unit-factor-resid's remaining built-ins.  Its default arm
 * is FACERR.  Row 8's scaffold drop DELETED the whole lift trunk, since
 * nothing referred to it once the list of remaining cases was empty. */
void basicvfp_hand_EXPR(struct ros_cpu *s);
void basicvfp_hand_EXPRRECUR(struct ros_cpu *s);
void basicvfp_hand_EXPRNEXT(struct ros_cpu *s);
void basicvfp_hand_EXPRHARD(struct ros_cpu *s);
void basicvfp_hand_AEEXPR(struct ros_cpu *s);
void basicvfp_hand_AECHAN(struct ros_cpu *s);
void basicvfp_hand_CHAN(struct ros_cpu *s);
void basicvfp_hand_CHANNL(struct ros_cpu *s);
void basicvfp_hand_ARLOOKCACHE(struct ros_cpu *s);
void basicvfp_hand_QSTR(struct ros_cpu *s);
void basicvfp_hand_VARIND(struct ros_cpu *s);
void basicvfp_hand_VARBYT(struct ros_cpu *s);
void basicvfp_hand_VARNOTNUM(struct ros_cpu *s);
void basicvfp_hand_VARSTR(struct ros_cpu *s);
void basicvfp_hand_BRA(struct ros_cpu *s);
void basicvfp_hand_VALSTR(struct ros_cpu *s);
void basicvfp_hand_VAL0(struct ros_cpu *s);
void basicvfp_hand_RNUL(struct ros_cpu *s);
void basicvfp_hand_RNULX(struct ros_cpu *s);
void basicvfp_hand_DATAST(struct ros_cpu *s);
void basicvfp_hand_INTEXA(struct ros_cpu *s);
void basicvfp_hand_INTEXC(struct ros_cpu *s);
void basicvfp_hand_EQAEEX(struct ros_cpu *s);
void basicvfp_hand_AEEXDN(struct ros_cpu *s);
void basicvfp_hand_EXPRDN(struct ros_cpu *s);
void basicvfp_hand_FACTOR(struct ros_cpu *s);

/* The lift's own FACTOR trunk under its swap name.  substitute.py
 * renames the definition and declares it next to the thunk.  Since
 * unit-factor-resid landed, no hand path tail-calls it, because the
 * dispatcher's default arm is FACERR.  Row 8's scaffold drop has
 * deleted the whole trunk. */

#endif
