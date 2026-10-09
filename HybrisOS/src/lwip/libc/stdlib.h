/* stdlib.h: lwIP uses only atoi from this header, to read the interface
 * number in netif_find. */
#ifndef HAL_STDLIB_H
#define HAL_STDLIB_H
static inline int atoi(const char *s)
{
    int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}
#endif
