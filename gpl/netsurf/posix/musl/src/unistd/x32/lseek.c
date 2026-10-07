/* lseek -- ROSGD overlay: upstream's inline syscall, through the gate.  The
 * gate returns the whole 64-bit offset (abi/x32/crt/gate.c). */
#include <unistd.h>
#include "syscall.h"

off_t __lseek(int fd, off_t offset, int whence)
{
	off_t ret = __rosgd_syscall(SYS_lseek, fd, offset, whence, 0, 0, 0);
	return ret < 0 ? __syscall_ret(ret) : ret;
}

weak_alias(__lseek, lseek);
