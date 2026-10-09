/* string.h: the C library functions that lwIP uses.  The HAL provides its
 * own versions of them. */
#ifndef HAL_STRING_H
#define HAL_STRING_H
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *h, const char *n);
#endif
