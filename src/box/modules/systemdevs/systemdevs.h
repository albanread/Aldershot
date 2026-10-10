/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* systemdevs.h -- SystemDevices, a native module (systemdevs.c): RISC OS
 * 5.30's name and version, so that stock !Run files' RMEnsure passes, and
 * its filing systems null:, vdu:, rawvdu:, kbd:, rawkbd: and source: (#136). */
#ifndef ROSGD_SYSTEMDEVS_H
#define ROSGD_SYSTEMDEVS_H

extern struct ros_module systemdevs_module;

/* Its filing systems (FileSwitch's table, modfs.c) */
struct fs;
extern const struct fs ros_nullfs, ros_vdufs, ros_rawvdufs, ros_kbdfs, ros_rawkbdfs, ros_sourcefs;

#endif
