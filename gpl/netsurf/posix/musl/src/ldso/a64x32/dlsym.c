/* dlsym -- A64X32 (ROSGD overlay, design 26): upstream's aarch64 dlsym.s,
 * which passes its caller's address to __dlsym, in C. */
#include <dlfcn.h>
#include "features.h"

hidden void *__dlsym(void *restrict, const char *restrict, void *restrict);

void *dlsym(void *restrict p, const char *restrict s)
{
	return __dlsym(p, s, __builtin_return_address(0));
}
