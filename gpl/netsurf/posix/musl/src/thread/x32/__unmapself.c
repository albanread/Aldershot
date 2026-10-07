/* __unmapself -- ROSGD overlay: a detached thread unmapping its own stack
 * and exiting.  No second thread exists yet (design 22, U-e), so this is
 * never reached; it exits the thread through the gate if it is. */
#include "pthread_impl.h"
#include "syscall.h"

hidden void __unmapself(void *base, size_t size)
{
	__syscall(SYS_munmap, base, size);
	for (;;) __syscall(SYS_exit, 0);
}
