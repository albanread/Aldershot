/* pthread_arch.h -- the thread pointer, without TPIDR_EL0.
 *
 * ROSGD overlay (design 22 section 9, design 26): x18 is design 20's static
 * base and TPIDR_EL0 the runtime's, so the program's thread pointer is an
 * ordinary variable, as on x32.  It is per task: a task's memory is at
 * &8000 while its code runs.  One thread per task until threads (U-e). */

extern hidden uintptr_t __rosgd_tp;

static inline uintptr_t __get_tp()
{
	return __rosgd_tp;
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 16

#define MC_PC pc
