/* syscall_arch.h -- musl's x32 system calls, made through ROSGD's Unix gate.
 *
 * ROSGD overlay (rosgd/posix, design 22): upstream's inline `syscall`
 * becomes a call to __rosgd_syscall (abi/x32/crt/gate.c), which makes
 * XUnix_Syscall through the C applications' SWI gate.  The numbers are
 * x32's (the x32 bit set), the arguments 64-bit as upstream passes them.
 * Nothing else here changes. */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

#define __scc(X) sizeof(1?(X):0ULL) < 8 ? (unsigned long) (X) : (long long) (X)
typedef long long syscall_arg_t;

hidden long long __rosgd_syscall(long long n, long long a1, long long a2, long long a3,
                                 long long a4, long long a5, long long a6);

static __inline long __syscall0(long long n)
{
	return __rosgd_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long long n, long long a1)
{
	return __rosgd_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long long n, long long a1, long long a2)
{
	return __rosgd_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long long n, long long a1, long long a2, long long a3)
{
	return __rosgd_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long long n, long long a1, long long a2, long long a3,
                                     long long a4)
{
	return __rosgd_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long long n, long long a1, long long a2, long long a3,
                                     long long a4, long long a5)
{
	return __rosgd_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long long n, long long a1, long long a2, long long a3,
                                     long long a4, long long a5, long long a6)
{
	return __rosgd_syscall(n, a1, a2, a3, a4, a5, a6);
}

#undef SYS_futimesat

#define SYS_clock_gettime64 SYS_clock_gettime
#define SYS_clock_settime64 SYS_clock_settime
#define SYS_clock_adjtime64 SYS_clock_adjtime
#define SYS_clock_nanosleep_time64 SYS_clock_nanosleep
#define SYS_timer_gettime64 SYS_timer_gettime
#define SYS_timer_settime64 SYS_timer_settime
#define SYS_timerfd_gettime64 SYS_timerfd_gettime
#define SYS_timerfd_settime64 SYS_timerfd_settime
#define SYS_utimensat_time64 SYS_utimensat
#define SYS_pselect6_time64 SYS_pselect6
#define SYS_ppoll_time64 SYS_ppoll
#define SYS_recvmmsg_time64 SYS_recvmmsg
#define SYS_mq_timedsend_time64 SYS_mq_timedsend
#define SYS_mq_timedreceive_time64 SYS_mq_timedreceive
#define SYS_semtimedop_time64 SYS_semtimedop
#define SYS_rt_sigtimedwait_time64 SYS_rt_sigtimedwait
#define SYS_futex_time64 SYS_futex
#define SYS_sched_rr_get_interval_time64 SYS_sched_rr_get_interval
#define SYS_getrusage_time64 SYS_getrusage
#define SYS_wait4_time64 SYS_wait4

#define IPC_64 0
