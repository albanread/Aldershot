/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* worker.h: Worker runs computation jobs on the cores that the tasks do not
 * use (modules/worker; README.md there lists the SWIs).
 */
#ifndef ROSGD_WORKER_H
#define ROSGD_WORKER_H

#include <stdint.h>

extern struct ros_module worker_module;

#define WORKER_VERSION 300u

/* The job record is in the RMA, and the handle is its address. The module
 * writes it. A program may read it, but Worker_Status and Worker_Result are
 * the proper way to. */
#define WJOB_MAGIC     0x424F4A57u      /* "WJOB" */
#define WJOB_MAGIC_OFF 0
#define WJOB_STATUS    4
#define WJOB_KERNEL    8
#define WJOB_TAG       12
#define WJOB_POLLWORD  16
#define WJOB_TASK      20
#define WJOB_ERRNUM    24               /* a failed job's error number, else 0 */
#define WJOB_ERRADDR   28               /* the address it faulted on */
#define WJOB_RESULTS   32               /* eight words */
#define WJOB_ARGS      64               /* sixteen words, as submitted */
#define WJOB_CANCEL    128              /* non-zero: asked to stop (the third
                                         * argument of an application kernel,
                                         * added with Worker_LoadKernel) */
#define WJOB_SIZE      144

#define WORKER_MAX_ARGS    16u
#define WORKER_MAX_RESULTS 8u

/* Statuses */
enum { WJOB_QUEUED, WJOB_RUNNING, WJOB_DONE, WJOB_FAILED, WJOB_CANCELLED };

/* Kernels: the built-in ones */
#define WORKER_KERNEL_MANDEL_ROW 1u

/* mandel_row's arguments (words) */
enum {
    MR_BUFFER,          /* the row: width words, RMA or a dynamic area */
    MR_WIDTH,           /* pixels, 1 to 16384 */
    MR_MAXITER,         /* 1 to 1048576 */
    MR_FLAGS,           /* bits 0-1 the output; bit 2 the coordinates are doubles */
    MR_PALETTE,         /* output 2: maxiter + 1 words, RMA or a dynamic area */
    MR_X0, MR_X0b,      /* the first pixel's real part */
    MR_DX, MR_DXb,      /* the step between pixels */
    MR_Y, MR_Yb,        /* the row's imaginary part */
    MR_NARGS
};
#define MR_OUT_COUNTS  0u               /* the iteration count, maxiter inside */
#define MR_OUT_COLOUR  1u               /* &00BBGGRR, a smooth ramp; black inside */
#define MR_OUT_PALETTE 2u               /* palette[count] */
#define MR_DOUBLES     4u               /* coordinates as IEEE doubles, low word
                                         * first; else 32.32 fixed point: the
                                         * integer part (signed), then the
                                         * fraction (unsigned) */

/* Errors: &C0180 up, as the chunk */
#define WORKER_ERRBASE 0xC0180u
enum {
    WE_BADJOB, WE_BADKERNEL, WE_BADARGS, WE_BADBUFFER, WE_NOTDONE, WE_CANCELLED,
    WE_FAULT, WE_TOOMANY, WE_BADRANGE, WE_BADPOLLWORD, WE_NOTHREADS, WE_BADREASON,
    WE_NOTWINDOW, WE_NOSPACE, WE_BADCODE, WE_NOKERNELS
};

/* Application kernels (added with Worker_LoadKernel): its R0 flags */
#define WLK_FILE       1u               /* R1 -> a kernel file (roscc link --kernel) */

/* A kernel file is this header, then the image. It is linked to be loaded
 * with the header at a page boundary, so the header's own address is the
 * image's base. The code is [WKF_HEADER, WKF_HEADER + code length). What
 * follows it, to the file's end, is read-only data. */
#define WKF_MAGIC      0x4E524B57u      /* "WKRN" */
#define WKF_MACHINE    4                /* the ELF machine: 62 x86-64, 183 AArch64 */
#define WKF_ENTRY      8                /* the entry's offset from the header */
#define WKF_CODE       12               /* the code's length, from WKF_HEADER */
#define WKF_LENGTH     16               /* the file's length, the header included */
#define WKF_VERSION    20               /* 1 */
#define WKF_HEADER     32

/* A buffer that an application kernel's arguments name (R4 and R5 hold two
 * each, the low half first). Bit 15 is set. Bits 0-3 give the argument
 * that holds its address. Bits 4-7 give the argument that holds its size in
 * units. Bits 8-9 give log2 of the unit. */
#define WKB_PRESENT    0x8000u

#define WORKER_MAX_KERNEL (1u << 20)    /* a kernel's image, at most */

/* Worker_ShareMemory's reasons (R0) */
#define WSM_SHARE     0u                /* R1 the start, R2 the length */
#define WSM_UNSHARE   1u                /* R1 an address in the window */
#define WSM_UNSHARE_ALL 2u              /* every window of the caller's */

#endif
