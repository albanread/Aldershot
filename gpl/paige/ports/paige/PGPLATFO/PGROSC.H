/* PGROSC.H -- the RISC OS port's platform shims, force-included before
 * every engine file.  Paige's CPUDEFS.H (edited for ROSC_COMPILE) maps
 * the platform; this header supplies what the engine expects from
 * "machinespecific" code: the Mac fixed-point helpers as plain C, and
 * errno names the freestanding target lacks.
 *
 * This file, with PGROSC.C, is the whole of the RISC OS platform layer:
 * memory, files, fonts and drawing are installable procs the engine
 * calls through pointers (PGHEADER/DEFPROCS.H), which PGROSC.C provides.
 */

#ifndef PGROSC_H
#define PGROSC_H

/* errno: the Box's freestanding headers don't define these */
#ifndef ENOSPC
#define ENOSPC 28
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EBADF
#define EBADF 9
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EPERM
#define EPERM 1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif

/* Mac Toolbox fixed point (16.16), as the engine's MACHINE.H expects
 * when it doesn't inline them away.  A64X32 has no problem with these
 * in plain C. */
#define FixMul(a, b) ((long)(((long long)(a) * (long long)(b)) >> 16))
#define FixDiv(a, b) ((long)(((long long)(a) << 16) / (long long)(b)))
#define FixRatio(n, d) ((long)(((long long)(n) << 16) / (long long)(d)))

#endif
