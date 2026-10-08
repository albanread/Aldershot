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

/* dispat_swap.c -- the statement dispatcher, translated by hand. This
 * is the scaffold that replaces the lift's basicvfp_DISPAT as a whole.
 *
 * This is what the lift's dispatcher does at its DISPAT label
 * (Basic.s:141 onwards). In the lift it is the switch after the
 * prologue.
 *
 *     DISPAT: LDR R4,[PC,R10,LSL #2]     ; the jump-table word -> R4
 *             ADD PC,PC,R4               ; switch on the token R10
 *     TWOSTMT: LDRB R10,[LINE],#1        ; the two-byte statement
 *             CMP R10,#TTWOSTMTLIMIT     ; trunk: extension byte,
 *             BCS ERSYNT                 ; bounds [142,164), then
 *             SUBS R4,R10,#&8E           ; the second table's word
 *             BCC ERSYNT                 ; -> R4 and its switch
 *             LDR R4,[PC,R4,LSL #2]
 *             ADD PC,PC,R4
 *
 * This unit does the same by hand. It reads the token from R10. Four
 * groups of statements have hand functions:
 *   A units/dispat_assign.c,  B units/dispat_control.c,
 *   C units/dispat_io.c,      D units/dispat_sys.c.
 * For each such statement it calls the function with ROS_TAIL_CALL,
 * under the one-argument CASE_SUBS contract. Statements never return.
 * The live return address is the current R14, which no arm here
 * touches. This matches the way the substituted case arms store their
 * entry R14 back. Every other token, meaning the residual targets and
 * every case that is not a statement, goes by ROS_TAIL_CALL to
 * basicvfp_DISPAT_lift. That is the lift's own dispatcher, renamed by
 * substitute.py and otherwise unchanged. So the fallback path behaves
 * exactly as the lift does, byte for byte. THE INVARIANT: with no
 * statements covered, this function is one table read and a tail call
 * into the lift's dispatch, and the gates pass unchanged.
 *
 * What each covered arm does, and why:
 *   - R4 := the jump-table word. The original's LDR leaves the word in
 *     R4 before every body. The substituted case arms store their
 *     local r4 (the word) to R[4]. So every landed hand statement has
 *     passed its gates with that value present. The arm here repeats
 *     the load and stores it. The address is 0xFC100BF4 + token*4 for
 *     the first switch, and 0xFC101010 + (ext-142)*4 for the second.
 *     No other R[] cell can differ. The dispatcher is entered only at
 *     function entry: from hand STMT's call, from the lift's DC entry,
 *     or by a code-table resume at &FC100BEC. There the lift's own
 *     prologue would load the very same R[] cells. The lift's INTERNAL
 *     branches back into its switch (THENLN's BNE DISPAT) never pass
 *     through here. They stay in the lift, whose CASE_SUBS arms stay
 *     live for them.
 *   - ROS_POLL is called on the LETST, LETSTNOTCACHE and DOSTAR tokens.
 *     The box build's lift writes exactly those cases with the
 *     lifter's checkpoint prefix (33,36,37-41,42,43-57,59,60,62,63,
 *     65-90,92-122,124). The poll changes behaviour, so it must be
 *     kept. The harness lift has no polls, and its cpu.h has no
 *     ROS_POLL. Hence the guard, which keeps each build faithful to
 *     its own lift.
 *   - TWOSTMT is decoded here and is not passed on to the lift. The
 *     extension byte is PEEKED (ros_ld8 of R12, with no advance). Only
 *     a COVERED word from the second table advances LINE and updates
 *     R10 and R4. An extension that is uncovered or out of range falls
 *     back to the lift with LINE untouched. The lift's own TWOSTMT then
 *     reads it again, checks it again and raises ERSYNT exactly as
 *     before.
 *
 * Coverage of the 85 dispatch targets (tools/dispat-map.md): 80 have
 * hand functions. A has 16, B 20, C 28 and D 16. The residual five
 * stay with the lift:
 *   - ASS (91);
 *   - CLOSE (217);
 *   - PROC (242);
 *   - TWOSTMT itself, the trunk. Case 200 arrives here only to be
 *     decoded;
 *   - CURSOFF (135). Its body lives in dispat_control.c in the old
 *     two-argument shape. Once it is brought to the one-argument
 *     CASE_SUBS shape, an arm here and a CASE_SUBS entry will land it.
 * Every case that is not a target (ERSYNT's ranges, CRLINE 13, STMT
 * 32/58) also falls to the lift's own arms.
 *
 * Compile-time wiring: the arms of each group compile only when
 * gen/dispat_swap_cfg.h defines that group's BASICVFP_DISPAT_GROUP_x.
 * substitute.py writes that file on every run, from manifest.DISPAT_SWAP
 * and from whether the group's unit file is present. The whole
 * dispatcher compiles only under BASICVFP_DISPAT_SWAP. If there is no
 * cfg file, or the swap is off, this unit is empty and the lift's
 * dispatcher is left untouched.
 *
 * The dispatch case numbers are the lift's own. The manifests and the
 * group headers carry them. The case values in the second switch are
 * the WORDS of the second jump table, read from the guest at run time
 * exactly as the lift reads them. Regenerate this map when the lift is
 * regenerated.
 */

#include "rosgd/cpu.h"
#include "rosgd/error.h"
#include "rosgd/arena.h"

#include "dispat_swap.h"

/* The swap's compile-time wiring. substitute.py generates it into the
 * shared gen directory (BASIC_T3_ROOT/gen). It is absent when
 * substitute.py has not run, and that means the swap is off. */
#if defined(__has_include)
# if __has_include("dispat_swap_cfg.h")
#  include "dispat_swap_cfg.h"
# endif
#endif
#ifndef BASICVFP_DISPAT_SWAP
# define BASICVFP_DISPAT_SWAP 0
#endif
#ifndef BASICVFP_DISPAT_GROUP_A
# define BASICVFP_DISPAT_GROUP_A 0
#endif
#ifndef BASICVFP_DISPAT_GROUP_B
# define BASICVFP_DISPAT_GROUP_B 0
#endif
#ifndef BASICVFP_DISPAT_GROUP_C
# define BASICVFP_DISPAT_GROUP_C 0
#endif
#ifndef BASICVFP_DISPAT_GROUP_D
# define BASICVFP_DISPAT_GROUP_D 0
#endif
#define DISPAT_SWAP_ANY_GROUP \
    (BASICVFP_DISPAT_GROUP_A || BASICVFP_DISPAT_GROUP_B || \
     BASICVFP_DISPAT_GROUP_C || BASICVFP_DISPAT_GROUP_D)

/* The token constants this unit reads. They are the lift's own values.
 * The twin defines the same macros to the same tokens, so the
 * redefinition is harmless. */
#define TESCSTMT 200u
#define TTWOSTMTLIMIT 164u
#define TFIRSTEXT 142u

/* The transfer contract, for when the unit is compiled outside the
 * twin. The hosted build compiles the units as separate translation
 * units. In the box build the twin defines ROS_TAIL_CALL identically,
 * before the units are concatenated. */
#ifndef ROS_TAIL_CALL
# if defined(__clang__)
#  define ROS_TAIL_CALL(f) __attribute__((musttail)) return f(s);
# elif defined(__GNUC__) && __GNUC__ >= 15
#  define ROS_TAIL_CALL(f) return __attribute__((musttail)) f(s);
# else
#  define ROS_TAIL_CALL(f) return f(s);
# endif
#endif

/* The lifter's checkpoint, where the tree's cpu.h has it. Where it does
 * not, this does nothing, because the harness lift has no polls to
 * keep. */
#ifndef ROS_POLL
# define ROS_POLL(s, sp) do { } while (0)
#endif

#if BASICVFP_DISPAT_SWAP

/* The lift's own dispatcher. substitute.py renames it when, and only
 * when, this unit is compiled. Uncovered tokens go this way. */
void basicvfp_DISPAT_lift(struct ros_cpu *s);

/* Groups A and B have landed (units/dispat_assign.c,
 * units/dispat_control.c), so their headers are always present. */
#include "dispat_assign.h"
#include "dispat_control.h"

/* Groups C and D (units/dispat_io.c, units/dispat_sys.c) land on
 * their own branches. Their functions are declared here, under the
 * same cfg gates as the arms that call them, so this unit compiles in
 * any tree. Once they have landed, these declarations repeat the
 * identical prototypes in their headers. That is legal C, and there is
 * one definition at link time. */
#if BASICVFP_DISPAT_GROUP_C
void basicvfp_hand_PRINT(struct ros_cpu *s);
void basicvfp_hand_VDU(struct ros_cpu *s);
void basicvfp_hand_INPUT(struct ros_cpu *s);
void basicvfp_hand_LINEST(struct ros_cpu *s);
void basicvfp_hand_READ(struct ros_cpu *s);
void basicvfp_hand_RESTORE(struct ros_cpu *s);
void basicvfp_hand_SOUND(struct ros_cpu *s);
void basicvfp_hand_STEREO(struct ros_cpu *s);
void basicvfp_hand_VOICE(struct ros_cpu *s);
void basicvfp_hand_VOICES(struct ros_cpu *s);
void basicvfp_hand_BEATS(struct ros_cpu *s);
void basicvfp_hand_TEMPO(struct ros_cpu *s);
void basicvfp_hand_MODES(struct ros_cpu *s);
void basicvfp_hand_COLOUR(struct ros_cpu *s);
void basicvfp_hand_GCOL(struct ros_cpu *s);
void basicvfp_hand_MOVE(struct ros_cpu *s);
void basicvfp_hand_DRAW(struct ros_cpu *s);
void basicvfp_hand_CIRCLE(struct ros_cpu *s);
void basicvfp_hand_ELLIPSE(struct ros_cpu *s);
void basicvfp_hand_FILL(struct ros_cpu *s);
void basicvfp_hand_PLOT(struct ros_cpu *s);
void basicvfp_hand_RECT(struct ros_cpu *s);
void basicvfp_hand_DOTINT(struct ros_cpu *s);
void basicvfp_hand_PSET(struct ros_cpu *s);
void basicvfp_hand_CLS(struct ros_cpu *s);
void basicvfp_hand_CLG(struct ros_cpu *s);
void basicvfp_hand_CLEAR(struct ros_cpu *s);
void basicvfp_hand_WAIT(struct ros_cpu *s);
#endif
#if BASICVFP_DISPAT_GROUP_D
void basicvfp_hand_DOSTAR(struct ros_cpu *s);
void basicvfp_hand_OSCL(struct ros_cpu *s);
void basicvfp_hand_SYS(struct ros_cpu *s);
void basicvfp_hand_CALL(struct ros_cpu *s);
void basicvfp_hand_LIBRARY(struct ros_cpu *s);
void basicvfp_hand_INSTALLBAD(struct ros_cpu *s);
void basicvfp_hand_OVERLAY(struct ros_cpu *s);
void basicvfp_hand_ENVEL(struct ros_cpu *s);
void basicvfp_hand_REPORT(struct ros_cpu *s);
void basicvfp_hand_LERROR(struct ros_cpu *s);
void basicvfp_hand_OTHER(struct ros_cpu *s);
void basicvfp_hand_WIDTH(struct ros_cpu *s);
void basicvfp_hand_ORGIN(struct ros_cpu *s);
void basicvfp_hand_DOMOUSE(struct ros_cpu *s);
void basicvfp_hand_BBPUT(struct ros_cpu *s);
void basicvfp_hand_FNRET(struct ros_cpu *s);
#endif

/* The dispatcher (DISPAT at Basic.s:141, and the lift's switch of 173
 * cases). The groups are tried with the busiest first: A (the LET
 * family), B (control flow), C (I/O and graphics) and D (system). Then
 * comes the two-byte statement trunk, and then the lift. A covered arm
 * never returns. A group that does not cover the token costs one
 * compare and falls through. */
void basicvfp_hand_DISPAT(struct ros_cpu *s)
{
#if DISPAT_SWAP_ANY_GROUP
    uint32_t tok = s->r[10];

    /* LDR R4,[PC,R10,LSL #2]: the jump-table word that the original
     * leaves in R4 for the body it is about to enter. A covered arm
     * stores it. Nothing else in R[] can differ at dispatcher entry. */
    uint32_t w4 = ros_ld32(0xFC100BF4u + (tok << 2));

    /* ---- Group A: the assignment family (units/dispat_assign.c).
     * These are first-switch cases. SWAP is in the second switch,
     * below. LETST covers the range of characters that can start an
     * identifier (80 cases). LETSTNOTCACHE and that range are the
     * box-build lift's polled cases. */
# if BASICVFP_DISPAT_GROUP_A
    switch (tok) {
    case 33: case 36: case 63: case 124:
        ROS_POLL(s, s->r[13]);
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LETSTNOTCACHE);
    case 37: case 38: case 39: case 40: case 41:
    case 43: case 44: case 45: case 46: case 47:
    case 48: case 49: case 50: case 51: case 52:
    case 53: case 54: case 55: case 56: case 57:
    case 59: case 60: case 62:
    case 65: case 66: case 67: case 68: case 69:
    case 70: case 71: case 72: case 73: case 74:
    case 75: case 76: case 77: case 78: case 79:
    case 80: case 81: case 82: case 83: case 84:
    case 85: case 86: case 87: case 88: case 89:
    case 90:
    case 92: case 93: case 94: case 95: case 96:
    case 97: case 98: case 99: case 100: case 101:
    case 102: case 103: case 104: case 105: case 106:
    case 107: case 108: case 109: case 110: case 111:
    case 112: case 113: case 114: case 115: case 116:
    case 117: case 118: case 119: case 120: case 121:
    case 122:
        ROS_POLL(s, s->r[13]);
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LETST);
    case 64:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_ASSIGNAT);
    case 143: case 207:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LPTR);
    case 144: case 208:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LPAGE);
    case 145: case 209:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LTIME);
    case 146: case 210:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LLOMEM);
    case 147: case 211:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LHIMEM);
    case 162:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LEXT);
    case 192:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LLEFTD);
    case 193:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LMIDD);
    case 194:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LRIGHTD);
    case 222:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_DIM);
    case 233:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LET);
    case 234:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LOCAL);
    default:
        break;
    }
# endif

    /* ---- Group B: the control-flow statements
     * (units/dispat_control.c). These are first-switch cases. CASE,
     * WHILE and QUIT are in the second switch, below. */
# if BASICVFP_DISPAT_GROUP_B
    switch (tok) {
    case 204:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_ELSE2);
    case 206:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_ENDWH);
    case 215:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_CHAIN);
    case 224:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_END);
    case 225:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_ENDPR);
    case 227:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_FOR);
    case 228:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_GOSUB);
    case 229:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_GOTO);
    case 231:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_IF);
    case 237:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_NEXT);
    case 238:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_ON);
    case 245:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_REPEAT);
    case 248:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_RETURN);
    case 249:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_RUN);
    case 250:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_STOP);
    case 252:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_TRACE);
    case 253:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_UNTIL);
    default:
        break;
    }
# endif

    /* ---- Group C: the I/O and graphics statements
     * (units/dispat_io.c). These are first-switch cases. The twelve
     * second-switch statements are below. CURSOFF (135) is NOT
     * covered. Its body in dispat_control.c still has the old
     * two-argument shape (see the residual five in the head comment). */
# if BASICVFP_DISPAT_GROUP_C
    switch (tok) {
    case 134:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LINEST);
    case 212:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_SOUND);
    case 216:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_CLEAR);
    case 218:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_CLG);
    case 219:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_CLS);
    case 223:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_DRAW);
    case 230:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_GCOL);
    case 232:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_INPUT);
    case 235:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_MODES);
    case 236:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_MOVE);
    case 239:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_VDU);
    case 240:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_PLOT);
    case 241:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_PRINT);
    case 243:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_READ);
    case 247:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_RESTORE);
    case 251:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_COLOUR);
    default:
        break;
    }
# endif

    /* ---- Group D: the system statements (units/dispat_sys.c). These
     * are first-switch cases. The six second-switch statements are
     * below. DOSTAR (42) is one of the box-build lift's polled cases. */
# if BASICVFP_DISPAT_GROUP_D
    switch (tok) {
    case 42:
        ROS_POLL(s, s->r[13]);
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_DOSTAR);
    case 61:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_FNRET);
    case 127: case 201:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_OTHER);
    case 133:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_LERROR);
    case 213:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_BBPUT);
    case 214:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_CALL);
    case 226:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_ENVEL);
    case 246:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_REPORT);
    case 254:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_WIDTH);
    case 255:
        s->r[4] = w4;
        ROS_TAIL_CALL(basicvfp_hand_OSCL);
    default:
        break;
    }
# endif

    /* ---- TWOSTMT: the two-byte statement trunk. It is decoded here so
     * that the second-switch statements also dispatch to hand code. The
     * extension byte is PEEKED, and LINE is not advanced. Only a
     * covered word makes the trunk's own updates: R10 := ext, LINE +1,
     * and R4 := the second table's word. Anything else falls to the
     * lift with LINE untouched. That is an extension out of range, or a
     * word with no hand arm. The lift's TWOSTMT then reads it again,
     * checks it again and dispatches exactly as before. */
    if (tok == TESCSTMT) {
        uint32_t ext = ros_ld8(s->r[12]);
        if (ext - TFIRSTEXT < TTWOSTMTLIMIT - TFIRSTEXT) {
            uint32_t word = ros_ld32(0xFC101010u + ((ext - TFIRSTEXT) << 2));
            switch (word) {
# if BASICVFP_DISPAT_GROUP_A
            case 20988:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_SWAP);
# endif
# if BASICVFP_DISPAT_GROUP_B
            case 15528:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_CASE);
            case 21544:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_WHILE);
            case 28456:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_QUIT);
# endif
# if BASICVFP_DISPAT_GROUP_C
            case 20684:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_BEATS);
            case 20696:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_VOICES);
            case 20724:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_VOICE);
            case 20772:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_TEMPO);
            case 20784:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_STEREO);
            case 23000:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_CIRCLE);
            case 23756:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_ELLIPSE);
            case 24116:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_FILL);
            case 25908:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_PSET);
            case 25976:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_RECT);
            case 26228:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_DOTINT);
            case 26372:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_WAIT);
# endif
# if BASICVFP_DISPAT_GROUP_D
            case 25152:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_DOMOUSE);
            case 25820:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_ORGIN);
            case 26436:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_OVERLAY);
            case 27288:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_LIBRARY);
            case 27632:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_SYS);
            case 61076:
                s->r[10] = ext;
                s->r[12] += 1;
                s->r[4] = word;
                ROS_TAIL_CALL(basicvfp_hand_INSTALLBAD);
# endif
            default:
                break;
            }
        }
    }
#endif /* DISPAT_SWAP_ANY_GROUP */

    /* ---- the glue. These tokens are not statements, but they are
     * dispatched more often than any statement is. A space (32) and a
     * colon (58) go to STMT, and a carriage return (13) goes to CRLINE.
     * Every statement in every program is reached across one of the
     * three. They used to fall through to the lift's dispatcher below.
     * That re-runs its whole prologue to serve them. It does fifteen
     * R[] loads, builds a frame for its two hundred or so declared
     * locals, loads a jump-table word it does not use, and switches a
     * second time on the same token. This took nine per cent of the
     * time in two different kinds of program (a tight assignment loop
     * and a PROC-call loop). Handling them here took five to twelve per
     * cent off the bench.
     *
     * The arms are the lift's own three. R[4] is the only cell that can
     * differ, for the reason given for the covered arms above. Their
     * targets are the thunks that substitute.py leaves for STMT and
     * CRLINE. So they reach the hand STMT and the hand CRLINE wherever
     * those have landed, and the lift's own where they have not. The
     * invariant above still holds with no coverage. This block reads
     * the token itself, so it does not depend on the groups.
     *
     * (Merging the four group switches above into one was measured too.
     * A group D token walks A's jump table, then B's, then C's. Merging
     * made no difference at all, not even for a group D statement with
     * a trivial body, because clang already folds or predicts the
     * switches. The groups keep their own switches.) */
    {
        uint32_t glue = s->r[10];
        if (glue == 32 || glue == 58 || glue == 13) {
            s->r[4] = ros_ld32(0xFC100BF4u + (glue << 2));
            if (glue == 13)
                ROS_TAIL_CALL(basicvfp_CRLINE);
            ROS_TAIL_CALL(basicvfp_STMT);
        }
    }

    /* ---- the lift's own dispatch. This takes every residual target
     * (ASS, CLOSE, CURSOFF, PROC, and the TWOSTMT extensions that are
     * uncovered or out of range) and every case that is not a statement
     * (ERSYNT's ranges). The renamed lift runs its prologue and its
     * switch again, unchanged. With no statements covered above, this
     * is the whole dispatcher. */
    ROS_TAIL_CALL(basicvfp_DISPAT_lift);
}

#endif /* BASICVFP_DISPAT_SWAP */
