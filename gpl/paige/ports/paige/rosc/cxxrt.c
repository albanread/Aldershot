/* cxxrt.c -- what the PGTXR codecs, C++ compiled without exceptions or
 * RTTI, need of a C++ runtime: new and delete over the C library's heap,
 * and the trap a pure virtual call falls into.  (size_t is unsigned int
 * on both boxes' ILP32 ABIs: the mangled names take 'j'.) */
#include <stdlib.h>

void *_Znwj(unsigned int n) { return malloc(n ? n : 1); }          /* new */
void *_Znaj(unsigned int n) { return malloc(n ? n : 1); }          /* new[] */
void _ZdlPv(void *p) { free(p); }                                   /* delete */
void _ZdlPvj(void *p, unsigned int n) { (void)n; free(p); }         /* sized delete */
void _ZdaPv(void *p) { free(p); }                                   /* delete[] */
void _ZdaPvj(void *p, unsigned int n) { (void)n; free(p); }

void __cxa_pure_virtual(void)
{
    abort();
}
