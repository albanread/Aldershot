/* __restore_rt, __restore -- A64X32 (ROSGD overlay): a signal handler's
 * return, as the x32 overlay's (signal/x32/restore.c).  aarch64's
 * <signal.h> has SA_RESTORER, so sigaction names both, as upstream's
 * aarch64 restore.s defines both. */
#include "syscall.h"

hidden void __restore_rt(void)
{
	__syscall(SYS_rt_sigreturn);
}

hidden void __restore(void) __attribute__((alias("__restore_rt")));
