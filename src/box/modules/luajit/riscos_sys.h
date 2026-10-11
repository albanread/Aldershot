/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* riscos_sys.h: the seam between the riscos library (riscos.c) and the
 * machine it runs on.  RiscLua (Gavin Wraith) had this in ARM assembler
 * (src/s/sys); the same four operations, as C, answered by the box's own
 * SWI dispatch (sys_box.c).
 *
 * rdir reads one item from directory dir: first call with offset 0, the
 * next offset returned; 0 means nothing was read, -1 no more.  The item
 * lands in buf (256 bytes of the caller's own memory): the filetype is
 * the word at buf+20, and the leaf name the zero-terminated string at
 * buf+24.
 *
 * file_type returns the filetype of name: -1 untyped or missing, &1000
 * directory, &2000 application.  swi_str2num converts a SWI name to its
 * number, returning -1 (as unsigned) for failure.
 *
 * swi_args calls the SWI with up to eight arguments, writing the eight
 * registers back into out: numbers pass through as they are, strings
 * are copied to SWI-visible memory (the RMA) for the call's lifetime.
 * It returns the error block it failed with, or NULL. */
#ifndef RISCOS_SYS_H
#define RISCOS_SYS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int errn;
    char errmsg[252];
} oserror;

typedef struct {
    int is_string;                    /* 0: v is the register's value */
    int64_t v;                        /* a number */
    const char *s;                    /* a string, n bytes long */
    size_t n;
} riscos_arg;

extern int rdir(const char *dir, int *buf, int offset);
extern int file_type(const char *name);
extern unsigned int swi_str2num(const char *s);
extern oserror *swi_args(uint32_t swinum, const riscos_arg *a, int nargs,
                         int64_t out[8]);

#endif
