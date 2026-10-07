/* pthread_arch.h -- the thread pointer, without %fs.
 *
 * ROSGD overlay (design 22 section 9): %fs is the runtime's, whose gate keeps
 * per-thread state there, so the program's thread pointer is an ordinary
 * variable.  It is per task: a task's memory is at &8000 while its code
 * runs.  One thread per task until threads (U-e) give each its own. */

extern hidden uintptr_t __rosgd_tp;

static inline uintptr_t __get_tp()
{
	return __rosgd_tp;
}

#define MC_PC gregs[REG_RIP]

#define CANARY_PAD

#define tls_mod_off_t unsigned long long
