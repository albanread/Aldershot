/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* basicname.c: BASIC, a native module. It has the name and version of
 * RISC OS 5.30's BASIC module.
 *
 * Stock !Run files check for that module before they start. PipeDream's
 * does this:
 *
 *     RMEnsure BASIC 0.00 Error 0 PipeDream's Loader needs BASIC. Is it unplugged?
 *
 * The check fails if no module of that name exists. The box's BASIC is
 * 5.31's BASICVFP (modules/basicvfp), and *BASIC is an alias for it
 * (runtime/sysvars.c). 5.30's ROM has BASIC105 as the module "BASIC".
 * With this module in the ROM the line passes, as the RMEnsure lines for
 * FPEmulator do (#114).
 *
 * The title is "BASIC". The help string is 5.30's: "BBC BASIC V", a tab,
 * then "1.85 (03 Oct 2022)". The version that RMEnsure compares is 1.85.
 * There are no commands, so *BASIC stays the alias. There are no SWIs,
 * as 5.30's module has none. */
#include "basicname.h"
#include "rosgd/module.h"

struct ros_module basicname_module = {
    .title = "BASIC",
    .help = "BBC BASIC V\t1.85 (03 Oct 2022)",
};
