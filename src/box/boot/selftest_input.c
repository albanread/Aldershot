/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_input.c: the Input module, against the calls that
 * RISC OS's USB driver makes (modules/input/README.md). Events in RISC OS's
 * terms go in, as the platform delivers them from evdev. The checks read
 * what KeyV and PointerV were called with, which are the calls RISC OS's
 * USB driver makes.
 *
 * In the guest it also checks the platform's table from Linux key codes to
 * RISC OS's key numbers. The hosted build has no evdev and skips that.
 */
#include <string.h>

#include "drmvideo.h"
#include "input.h"
#include "rosgd/arena.h"
#include "rosgd/cpu.h"
#include "rosgd/platform.h"
#include "rosgd/vector.h"
#include "selftest.h"

#define check ros_check

#define KEYV 0x13u
#define POINTERV 0x26u
#define ABSO 0x6F736241u

static struct call {
    uint32_t r[5];
} calls[16];
static unsigned ncalls;
static uint32_t which[16];

static int record(uint32_t vector, struct ros_cpu *s)
{
    if (ncalls < 16) {
        which[ncalls] = vector;
        memcpy(calls[ncalls].r, s->r, sizeof calls[0].r);
    }
    ncalls++;
    return ROS_VECTOR_PASS;
}

static int keyv(struct ros_cpu *s, uint32_t r12) { (void)r12; return record(KEYV, s); }
static int pointerv(struct ros_cpu *s, uint32_t r12)
{
    (void)r12;
    return s->r[0] == 3 || s->r[0] == 9 ? record(POINTERV, s) : ROS_VECTOR_PASS;
}

static void in(enum ros_input_kind kind, uint32_t key, uint32_t axis, int32_t value)
{
    struct ros_input_event ev = { kind, key, axis, value, 0 };
    input_deliver(&ev);
}

static int is(unsigned i, uint32_t vector, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
              uint32_t r4)
{
    return i < ncalls && which[i] == vector && calls[i].r[0] == r0 && calls[i].r[1] == r1 &&
           calls[i].r[2] == r2 && calls[i].r[3] == r3 && calls[i].r[4] == r4;
}

static void pointer(uint32_t r[4])
{
    struct ros_cpu s;
    ros_cpu_enter(&s);
    memcpy(s.r, r, 4 * sizeof r[0]);
    ros_vector_call(POINTERV, &s);
    memcpy(r, s.r, 4 * sizeof r[0]);
}

void ros_selftest_input(void)
{
    ros_vector_claim_native(KEYV, keyv, 0);
    ros_vector_claim_native(POINTERV, pointerv, 0);

    /* ---- keys and buttons: KeyV 2 down, 1 up, by key number ---- */
    ncalls = 0;
    in(ROS_INPUT_KEY, 0x3C, 0, 1);                  /* A */
    in(ROS_INPUT_KEY, 0x3C, 0, 2);                  /* Linux's autorepeat */
    in(ROS_INPUT_KEY, 0x3C, 0, 0);
    in(ROS_INPUT_KEY, 0x70, 0, 1);                  /* the left button */
    check(ncalls == 3 && is(0, KEYV, 2, 0x3C, 0, 0, 0) && is(1, KEYV, 1, 0x3C, 0, 0, 0) &&
              is(2, KEYV, 2, 0x70, 0, 0, 0),
          "Input: keys and buttons -- KeyV KeyDown and KeyUp, repeats dropped", "%u calls",
          ncalls);
    in(ROS_INPUT_KEY, 0x70, 0, 0);

    /* ---- movement: PointerV Report, Y turned upwards ---- */
    ncalls = 0;
    in(ROS_INPUT_MOVE, 0, ROS_AXIS_X, 5);
    in(ROS_INPUT_MOVE, 0, ROS_AXIS_Y, 3);           /* 3 down, as Linux counts */
    in(ROS_INPUT_SYNC, 0, 0, 0);
    check(ncalls == 1 && is(0, POINTERV, 3, 7, 5, (uint32_t)-3, 0),
          "Input: movement -- PointerV Report, device 7, dx 5, dy -3", "R2 %d R3 %d",
          (int32_t)calls[0].r[2], (int32_t)calls[0].r[3]);
    uint32_t r[4] = { 0, 7, 0, 0 };
    pointer(r);
    uint32_t again[4] = { 0, 7, 0, 0 };
    pointer(again);
    check(r[2] == 5 && r[3] == (uint32_t)-3 && again[2] == 0 && again[3] == 0,
          "Input: PointerV Request answers the movement since the last, once", NULL);
    uint32_t other[4] = { 0, 3, 0x77, 0x77 };
    pointer(other);
    check(other[2] == 0x77, "Input: a Request for another device type is not ours", NULL);

    /* ---- a tablet: the position in OS units, Y upwards, "Abso" ---- */
    uint32_t sw = 1280 << 1, sh = 1024 << 1;
    const struct ros_display *d = drmvideo_display();
    if (d)
        sw = d->width << 1, sh = d->height << 1;
    ncalls = 0;
    in(ROS_INPUT_POSITION, 0, ROS_AXIS_X, 32768);
    in(ROS_INPUT_POSITION, 0, ROS_AXIS_Y, 0);       /* the top of the device */
    in(ROS_INPUT_SYNC, 0, 0, 0);
    check(ncalls == 1 && is(0, POINTERV, 3, 7, sw / 2, sh - 1, ABSO),
          "Input: a tablet -- PointerV Report with R4 \"Abso\", OS units, Y upwards",
          "x %u of %u, y %u of %u", calls[0].r[2], sw, calls[0].r[3], sh);

    /* ---- the wheel: PointerV 9, R1 negative away ---- */
    ncalls = 0;
    in(ROS_INPUT_WHEEL, 0, ROS_AXIS_Y, 1);
    in(ROS_INPUT_SYNC, 0, 0, 0);
    check(ncalls == 1 && is(0, POINTERV, 9, (uint32_t)-1, 0, 0, 0),
          "Input: the wheel -- PointerV WheelChange, one notch away", NULL);

    /* ---- Identify, and Selected ---- */
    uint32_t id[4] = { 1, 0x1234, 0, 0 };
    pointer(id);
    check(id[1] != 0x1234 && ros_ld32(id[1]) == 0x1234 && ros_ld8(id[1] + 8) == 7,
          "Input: PointerV Identify -- its record, at the head of the list", NULL);
    uint32_t sel[4] = { 2, 3, 0, 0 };
    pointer(sel);
    ncalls = 0;
    in(ROS_INPUT_MOVE, 0, ROS_AXIS_X, 1);
    in(ROS_INPUT_SYNC, 0, 0, 0);
    check(ncalls == 0, "Input: another device Selected -- no reports", NULL);
    sel[1] = 7;
    pointer(sel);
    in(ROS_INPUT_MOVE, 0, ROS_AXIS_X, 1);
    in(ROS_INPUT_SYNC, 0, 0, 0);
    check(ncalls == 1, "Input: Selected again -- reports again", NULL);

    /* ---- the platform's key numbers (Linux key codes, linux/input.h) ---- */
    if (ros_input_key_number(30) != -1)
        check(ros_input_key_number(30) == 0x3C && ros_input_key_number(1) == 0x00 &&
                  ros_input_key_number(107) == 0x35 && ros_input_key_number(86) == 0x4D &&
                  ros_input_key_number(0x110) == 0x70 && ros_input_key_number(0x111) == 0x72 &&
                  ros_input_key_number(240) == -1,
              "Input: evdev key codes as RISC OS key numbers -- A, Escape, End (Copy), the "
              "ISO key, the buttons", NULL);

    ros_vector_release_native(POINTERV, pointerv, 0);
    ros_vector_release_native(KEYV, keyv, 0);
}
