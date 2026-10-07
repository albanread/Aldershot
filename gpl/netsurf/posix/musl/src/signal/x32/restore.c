/* __restore_rt -- ROSGD overlay: a signal handler's return.  Signals are
 * delivered by the Unix bridge, which returns through the gate. */
#include "syscall.h"

hidden void __restore_rt(void)
{
	__syscall(SYS_rt_sigreturn);
}
