/* uart.c: the console. This drives the PL011 on QEMU's virt machine by
 * polling, and provides kprintf. */
#include <stdarg.h>

#include "hal.h"

#define UART ((volatile uint32_t *)pa_to_va(HAL_UART_PA))
#define DR 0                            /* data register */
#define FR 6                            /* flag register (offset &18, divided by 4) */
#define FR_RXFE (1u << 4)
#define FR_TXFF (1u << 5)

void uart_putc(int c)
{
    while (UART[FR] & FR_TXFF)
        ;
    UART[DR] = (uint32_t)c & 0xFF;
}

void uart_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            uart_putc('\r');
        uart_putc(s[i]);
    }
}

int uart_getc(void)
{
    if (UART[FR] & FR_RXFE)
        return -1;
    return (int)(UART[DR] & 0xFF);
}

/* ---- kprintf: %s %c %d %u %x %X %lx %ld %lu %p, with a width -----------
 *
 * Output goes to the console, or into a buffer (for ksnprintf). All output
 * passes through out(). */

static char *sink;                              /* ksnprintf's buffer, or 0 for the console */
static size_t sink_left, sink_len;

static void out(const char *s, size_t n)
{
    if (!sink) {
        uart_write(s, n);
        return;
    }
    for (size_t i = 0; i < n; i++, sink_len++)
        if (sink_left > 1)
            *sink++ = s[i], sink_left--;
}


static void put_num(uint64_t v, unsigned base, int neg, int width, char pad, int upper)
{
    char b[24];
    int n = 0;
    do {
        b[n++] = (upper ? "0123456789ABCDEF" : "0123456789abcdef")[v % base];
        v /= base;
    } while (v);
    if (neg)
        b[n++] = '-';
    while (n < width)
        b[n++] = pad;
    while (n)
        out(&b[--n], 1);
}

static void vkprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(fmt, 1);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, lng = 0;
        if (*fmt == '0')
            pad = '0', fmt++;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l')
            lng = 1, fmt++;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            s = s ? s : "(null)";
            out(s, strlen(s));
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            out(&c, 1);
            break;
        }
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(v < 0 ? (uint64_t)-v : (uint64_t)v, 10, v < 0, width, pad, 0);
            break;
        }
        case 'u':
            put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, 0, width, pad, 0);
            break;
        case 'x':
        case 'X':
            put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 0, width, pad, *fmt == 'X');
            break;
        case 'p':
            out("0x", 2);
            put_num((uint64_t)va_arg(ap, void *), 16, 0, 16, '0', 0);
            break;
        case '%':
            out("%", 1);
            break;
        default:
            out("%?", 2);
        }
    }
}

/* Format as kprintf does into buf, which holds n bytes, and terminate it.
 * Returns the length that the whole output needed. */
int ksnprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sink = buf, sink_left = n, sink_len = 0;
    vkprintf(fmt, ap);
    if (n)
        *sink = 0;
    sink = 0;
    va_end(ap);
    return (int)sink_len;
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    uart_write("HAL panic: ", 11);
    vkprintf(fmt, ap);
    va_end(ap);
    uart_write("\n", 1);
    psci_off();
}

/* ---- the few C library routines the HAL needs ----------------------- */

/* These are marked no_builtin. Without it, the compiler may turn each loop
 * into a call to the function itself. */
__attribute__((no_builtin)) void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

__attribute__((no_builtin)) void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b || a >= b + n) {
        for (size_t i = 0; i < n; i++)
            a[i] = b[i];
    } else {
        while (n--)
            a[n] = b[n];
    }
    return d;
}

__attribute__((no_builtin)) void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++)
        if (!strncmp(h, n, l))
            return (char *)h;
    return l ? 0 : (char *)h;
}
