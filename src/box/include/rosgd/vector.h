/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* vector.h -- the kernel's hook chains: software vectors, events,
 * callbacks and service calls, as the runtime keeps them.
 *
 * They follow RISC OS's own protocols (Kernel/s/ArthurSWIs, NewIRQs), so a
 * compiled claimant works unchanged.  It is entered with R12 its own value
 * and lr a return that passes the call on.  To claim the call it pulls the
 * return address that the vector's caller left on the stack.  Claimants
 * written in C sit in the same chains, beside compiled ones.
 */
#ifndef ROSGD_VECTOR_H
#define ROSGD_VECTOR_H

#include <stdint.h>

#include "rosgd/cpu.h"
#include "rosgd/error.h"

#define ROS_VECTORS 0x30u       /* &00-&2F, NVECTORS in the kernel */

enum {
    ROS_EVENTV  = 0x10,
    ROS_INSV    = 0x14,
    ROS_REMV    = 0x15,
    ROS_CNPV    = 0x16,
    ROS_TICKERV = 0x1C,
    ROS_UPCALLV = 0x1D,
    ROS_GRAPHICSV = 0x2A,
};

/* The return addresses a compiled claimant is given: lr passes the call on;
 * the word on the stack claims it.  No compiled code can produce either. */
#define ROS_VECTOR_PASSED  0xFFFFFFF4u
#define ROS_VECTOR_CLAIMED 0xFFFFFFF0u

/* A claimant written in C.  It sees the caller's registers and flags, and
 * returns ROS_VECTOR_CLAIM to end the call or ROS_VECTOR_PASS to hand it on,
 * as a compiled claimant pulls or does not pull the claim address. */
enum { ROS_VECTOR_PASS, ROS_VECTOR_CLAIM };
typedef int ros_vector_fn(struct ros_cpu *s, uint32_t r12);

/* OS_Claim (add = 0): the claimant goes to the head of the chain, any
 * identical claim being removed first.  OS_AddToVector (add = 1): to the
 * head, without removing anything.  code is an arena code address. */
os_error *ros_vector_claim(uint32_t vector, uint32_t code, uint32_t r12, int add);
os_error *ros_vector_release(uint32_t vector, uint32_t code, uint32_t r12);
os_error *ros_vector_claim_native(uint32_t vector, ros_vector_fn *fn, uint32_t r12);
os_error *ros_vector_release_native(uint32_t vector, ros_vector_fn *fn, uint32_t r12);

/* Call a vector with a register block, head first, until a claimant
 * claims: 1 if one did. Flags go in as the caller set them (InsV, RemV
 * and CnpV take their inputs in C and V) and come back as the claimant
 * left them. The chains have no default owner. A caller that has one, as
 * WrchV's has (ros_wrch_default), runs it when this returns 0. An
 * unclaimed call leaves the registers as they were. */
int ros_vector_call(uint32_t vector, struct ros_cpu *s);

/* Events.  One below 32 reaches EventV only while enabled (OS_Byte 14);
 * one of 32 or more always does.  C is clear on return if EventV was
 * called, set if the event was disabled (the kernel's OSEVEN). */
void ros_event_enable(uint32_t event, int on);
/* OS_Byte 14 (enable) and 13 (disable), exactly: the old semaphore back. */
uint32_t ros_event_semaphore(uint32_t event, int enable);
void ros_event_generate(struct ros_cpu *s);         /* R0 = event, R1... */

/* Transient callbacks (OS_AddCallBack): run when the runtime is next idle,
 * in the order they were added, by ros_callbacks_run(). */
os_error *ros_callback_add(uint32_t code, uint32_t r12);
os_error *ros_callback_remove(uint32_t code, uint32_t r12);
void ros_callback_add_native(void (*fn)(void *arg), void *arg);
void ros_callbacks_run(void);

#endif
