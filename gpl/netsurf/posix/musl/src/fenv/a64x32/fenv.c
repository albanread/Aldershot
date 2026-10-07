/* <fenv.h> on A64X32 -- ROSGD overlay (design 26): upstream's aarch64
 * fenv.s in C, so fegetenv's and fesetenv's accesses are formed by the
 * compiler, bounded as all of A64X32's are.  FPCR and FPSR as upstream. */
#include <fenv.h>
#include "features.h"

static inline unsigned long long get_fpcr(void)
{
	unsigned long long x;
	__asm__ __volatile__ ("mrs %0, fpcr" : "=r"(x));
	return x;
}

static inline void set_fpcr(unsigned long long x)
{
	__asm__ __volatile__ ("msr fpcr, %0" : : "r"(x));
}

static inline unsigned long long get_fpsr(void)
{
	unsigned long long x;
	__asm__ __volatile__ ("mrs %0, fpsr" : "=r"(x));
	return x;
}

static inline void set_fpsr(unsigned long long x)
{
	__asm__ __volatile__ ("msr fpsr, %0" : : "r"(x));
}

int fegetround(void)
{
	return get_fpcr() & 0xc00000;
}

hidden int __fesetround(int r)
{
	set_fpcr((get_fpcr() & ~0xc00000ull) | (unsigned)r);
	return 0;
}

int fetestexcept(int mask)
{
	return mask & 0x1f & (int)get_fpsr();
}

int feclearexcept(int mask)
{
	set_fpsr(get_fpsr() & ~(unsigned long long)(mask & 0x1f));
	return 0;
}

int feraiseexcept(int mask)
{
	set_fpsr(get_fpsr() | (unsigned)(mask & 0x1f));
	return 0;
}

int fegetenv(fenv_t *envp)
{
	envp->__fpcr = (unsigned)get_fpcr();
	envp->__fpsr = (unsigned)get_fpsr();
	return 0;
}

int fesetenv(const fenv_t *envp)
{
	unsigned long long c = 0, s = 0;
	if (envp != FE_DFL_ENV)
		c = envp->__fpcr, s = envp->__fpsr;
	set_fpcr(c);
	set_fpsr(s);
	return 0;
}
