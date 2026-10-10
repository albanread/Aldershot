/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* pdriver.h -- PDriver: RISC OS's printer driver interface, whose pages
 * come out as PDF (modules/pdriver; README.md there describes it).
 */
#ifndef ROSGD_PDRIVER_H
#define ROSGD_PDRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/module.h"

extern struct ros_module pdriver_module;

#define PDRIVER_VERSION 100u            /* 1.00, as PDriver_Info gives it */
#define PDRIVER_TYPE    64u             /* the driver's type: ours, not an
                                         * allocated one (hdr/PDriverReg
                                         * allocates 0-20) */
#define PDRIVER_DPI     180u            /* a page pixel is one OS unit */

/* The errors, ROSGD's own, in the application range as the other native
 * modules' are (sshd &C0100, BoxTools &C0120, Worker &C0180, VDisplay
 * &C01C0). */
#define PDRIVER_ERRBASE 0xC01E0u

/* The page, for *PrintInfo (pdriver.c) */
uint32_t pdriver_paper_x(void);
uint32_t pdriver_paper_y(void);

/* Where a job goes (dest.c).  printerfs.c calls these as a job is opened
 * and as it is closed; the destination is *PrintTo's. */
struct os_error;
os_error *pdriver_dest_open(const char *path, char *host, size_t hostmax,
                            char *shown, size_t shownmax);
os_error *pdriver_dest_done(const char *host, const char *shown, uint32_t bytes);

/* What the writer should make for the destination in force: 0 a PDF, 1 PWG
 * Raster at *dpi, grey if *gray. */
int pdriver_dest_format(uint32_t *dpi, int *gray);
/* The chosen printer's name (its model), or NULL when jobs go to a file */
const char *pdriver_dest_name(void);

/* The module's commands (dest.c) and "printer:" (printerfs.c) */
extern const struct ros_command pdriver_commands[];
extern const struct fs ros_printerfs;

#endif
