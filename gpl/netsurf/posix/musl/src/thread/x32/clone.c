/* __clone -- ROSGD overlay.  A new thread or process sharing the task's
 * memory is not made yet (design 22: threads, U-e; posix_spawn goes through
 * the gate instead of clone), so this says so. */
#include <errno.h>
#include "pthread_impl.h"

hidden int __clone(int (*func)(void *), void *stack, int flags, void *arg, ...)
{
	(void)func, (void)stack, (void)flags, (void)arg;
	return -ENOSYS;
}
