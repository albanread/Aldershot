/* __syscall_cp_asm -- ROSGD overlay: a cancellation-point system call.
 * Cancellation is checked before the call; the call goes through the gate. */
#include "pthread_impl.h"
#include "syscall.h"

/* The cancellable region's bounds, which the cancel signal's handler compares
 * the interrupted pc with: nothing is ever interrupted inside one here. */
hidden const char __cp_begin[1], __cp_end[1], __cp_cancel[1];

hidden long __cancel(void);

hidden long __syscall_cp_asm(volatile int *cancel, syscall_arg_t nr, syscall_arg_t u,
                             syscall_arg_t v, syscall_arg_t w, syscall_arg_t x,
                             syscall_arg_t y, syscall_arg_t z)
{
	if (*cancel)
		return __cancel();
	return __rosgd_syscall(nr, u, v, w, x, y, z);
}
