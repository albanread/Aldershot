/* __set_thread_area -- ROSGD overlay: the thread pointer is a variable
 * (arch/x32/pthread_arch.h), not %fs. */
#include <stdint.h>
#include "pthread_impl.h"

hidden uintptr_t __rosgd_tp;

hidden int __set_thread_area(void *p)
{
	__rosgd_tp = (uintptr_t)p;
	return 0;
}
