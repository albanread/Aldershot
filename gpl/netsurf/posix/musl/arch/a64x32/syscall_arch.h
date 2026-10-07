/* syscall_arch.h -- musl's system calls on A64X32, through ROSGD's Unix gate.
 *
 * ROSGD overlay (rosgd/posix, design 22 B8, design 26): the gate is the
 * same SWI, Unix_Syscall, with the same numbers -- x86-64's, the x32 bit set
 * (bits/syscall.h is x32's): they are the bridge's own numbering, not a
 * kernel's.  So the calls are x32's, word for word. */
#include "../x32/syscall_arch.h"
