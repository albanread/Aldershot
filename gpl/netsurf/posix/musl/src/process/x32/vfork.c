/* vfork -- ROSGD overlay.  A task cannot be copied; posix_spawn runs a
 * program through the gate instead (design 22, section 3.3). */
#include <errno.h>
#include <unistd.h>
#include "syscall.h"

pid_t vfork(void)
{
	return __syscall_ret(-ENOSYS);
}
