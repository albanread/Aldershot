/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* vfpmath.h: VFPSupport's elementary functions in double precision, as
 * RISC OS 5 computes them (elementary.c). Each takes the caller's CPU
 * state, which holds its FPSCR. The cosine function also uses R6: see
 * elementary.c.
 */
#ifndef ROSGD_VFPSUPPORT_VFPMATH_H
#define ROSGD_VFPSUPPORT_VFPMATH_H

struct ros_cpu;

double ros_vfp_sin(struct ros_cpu *s, double x);
double ros_vfp_cos(struct ros_cpu *s, double x);
double ros_vfp_tan(struct ros_cpu *s, double x);
double ros_vfp_asin(struct ros_cpu *s, double x);
double ros_vfp_acos(struct ros_cpu *s, double x);
double ros_vfp_atan(struct ros_cpu *s, double x);
double ros_vfp_atan2(struct ros_cpu *s, double y, double x);
double ros_vfp_log(struct ros_cpu *s, double x);
double ros_vfp_log10(struct ros_cpu *s, double x);
double ros_vfp_exp(struct ros_cpu *s, double x);
double ros_vfp_pow(struct ros_cpu *s, double x, double y);

#endif
