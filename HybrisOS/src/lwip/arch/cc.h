/* arch/cc.h: describes the HAL to lwIP.  There is no C library, the
 * processor is little-endian AArch64, and lwIP's diagnostics go to the
 * console. */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_TIMEVAL_PRIVATE 0

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

void kprintf(const char *fmt, ...);
__attribute__((noreturn)) void panic(const char *fmt, ...);
unsigned hal_random(void);

#define LWIP_PLATFORM_DIAG(x) do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) panic("lwIP: %s (%s:%d)", x, __FILE__, __LINE__)
#define LWIP_RAND() ((u32_t)hal_random())

#endif
