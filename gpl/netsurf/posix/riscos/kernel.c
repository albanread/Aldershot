/* kernel.c -- <kernel.h> and <swis.h> for POSIX applications (design 22,
 * U-g): RISC OS's generalised SWI call and the C library's OS interface,
 * over the C applications' SWI gate (__rosgd_swi, abi/x32/crt/gate.c).
 * ROSGD's own, written from the interfaces' documentation. */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <kernel.h>
#include <swis.h>

#define X_BIT         0x20000u
#define V_FLAG        (1u << 28)
#define C_FLAG        (1u << 29)
#define BLOCK_WORDS   16                /* the most a _BLOCK takes from the arguments */

enum { SWI_WriteC = 0x0, SWI_ReadC = 0x4, SWI_CLI = 0x5, SWI_Byte = 0x6, SWI_Word = 0x7, SWI_File = 0x8,
       SWI_Args = 0x9, SWI_BGet = 0xA, SWI_BPut = 0xB, SWI_GBPB = 0xC, SWI_Find = 0xD, SWI_GetEnv = 0x10,
       SWI_ReadVarVal = 0x23, SWI_SetVarVal = 0x24 };

__attribute__((visibility("hidden"))) unsigned long long __rosgd_swi(unsigned, const unsigned[10], unsigned[10]);

/* ---- errors ---------------------------------------------------------------- */

static _kernel_oserror last;
static int have_last;

/* An error a SWI returned: kept for _kernel_last_oserror, a copy because
 * the OS's own buffers are reused */
static _kernel_oserror *record(uint64_t ret)
{
    _kernel_oserror *e = (_kernel_oserror *)(uintptr_t)(uint32_t)ret;
    last.errnum = e->errnum;
    strncpy(last.errmess, e->errmess, sizeof last.errmess - 1);
    last.errmess[sizeof last.errmess - 1] = 0;
    have_last = 1;
    return e;
}

_kernel_oserror *_kernel_last_oserror(void)
{
    if (!have_last)
        return NULL;
    have_last = 0;
    return &last;
}

/* One SWI: the PSR in the result's high word, an error's address in its
 * low word if V is set */
static uint64_t swi(unsigned no, uint32_t r[10])
{
    uint64_t ret = __rosgd_swi(no, r, r);
    if ((uint32_t)(ret >> 32) & V_FLAG)
        record(ret);
    return ret;
}

#define FAILED(ret) ((uint32_t)((ret) >> 32) & V_FLAG)
#define CARRY(ret)  ((uint32_t)((ret) >> 32) & C_FLAG)

/* ---- _swi and _swix ---------------------------------------------------------- */

/* The mask's registers from the arguments, the SWI, then its outputs to the
 * arguments' addresses (not if it failed); the PSR back */
static uint64_t generic(unsigned no, unsigned mask, va_list ap, uint32_t r[10])
{
    uint32_t block[BLOCK_WORDS];
    unsigned *out[10] = { 0 }, *flags = NULL;
    for (int i = 0; i < 10; i++)
        r[i] = mask & (1u << i) ? va_arg(ap, unsigned) : 0;
    for (int i = 0; i < 10; i++)
        if (mask & (1u << (31 - i)))
            out[i] = va_arg(ap, unsigned *);
    if (mask & (1u << 21))
        flags = va_arg(ap, unsigned *);
    if (mask & (1u << 11)) {
        /* The arguments left, in a block the register points to.  How many
         * there are is the SWI's business: this takes BLOCK_WORDS, enough for
         * any block of the Wimp's or the OS's passed this way. */
        for (int i = 0; i < BLOCK_WORDS; i++)
            block[i] = va_arg(ap, unsigned);
        r[(mask >> 12) & 15] = (uint32_t)(uintptr_t)block;
    }
    uint64_t ret = swi(no, r);
    if (FAILED(ret))
        return ret;
    for (int i = 0; i < 10; i++)
        if (out[i])
            *out[i] = r[i];
    if (flags)
        *flags = (uint32_t)(ret >> 32);
    return ret;
}

_kernel_oserror *_vswix(int swi_no, unsigned int mask, va_list ap)
{
    uint32_t r[10];
    uint64_t ret = generic((unsigned)swi_no | X_BIT, mask, ap, r);
    return FAILED(ret) ? (_kernel_oserror *)(uintptr_t)(uint32_t)ret : NULL;
}

_kernel_oserror *_swix(int swi_no, unsigned int mask, ...)
{
    va_list ap;
    va_start(ap, mask);
    _kernel_oserror *e = _vswix(swi_no, mask, ap);
    va_end(ap);
    return e;
}

/* Without the X bit, an error does not come back here */
int _vswi(int swi_no, unsigned int mask, va_list ap)
{
    uint32_t r[10];
    uint64_t ret = generic((unsigned)swi_no & ~X_BIT, mask, ap, r);
    unsigned which = (mask >> 16) & 15;
    return which == 15 ? (int)(uint32_t)(ret >> 32) : (int)r[which < 10 ? which : 0];
}

int _swi(int swi_no, unsigned int mask, ...)
{
    va_list ap;
    va_start(ap, mask);
    int v = _vswi(swi_no, mask, ap);
    va_end(ap);
    return v;
}

/* ---- <kernel.h> -------------------------------------------------------------- */

int _kernel_hostos(void)
{
    return _kernel_ARTHUR;
}

static _kernel_oserror *kswi(int no, _kernel_swi_regs *in, _kernel_swi_regs *out, uint64_t *ret)
{
    unsigned n = (unsigned)no & ~(unsigned)_kernel_NONX;
    n = no & _kernel_NONX ? n & ~X_BIT : n | X_BIT;
    uint32_t r[10];
    memcpy(r, in->r, sizeof r);
    *ret = swi(n, r);
    if (FAILED(*ret))
        return (_kernel_oserror *)(uintptr_t)(uint32_t)*ret;
    memcpy(out->r, r, sizeof r);
    return NULL;
}

_kernel_oserror *_kernel_swi(int no, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    uint64_t ret;
    return kswi(no, in, out, &ret);
}

_kernel_oserror *_kernel_swi_c(int no, _kernel_swi_regs *in, _kernel_swi_regs *out, int *carry)
{
    uint64_t ret;
    _kernel_oserror *e = kswi(no, in, out, &ret);
    if (!e)
        *carry = CARRY(ret) != 0;
    return e;
}

char *_kernel_command_string(void)
{
    uint32_t r[10] = { 0 };
    swi(SWI_GetEnv | X_BIT, r);
    return (char *)(uintptr_t)r[0];
}

/* The int calls: _kernel_ERROR on an error, -1 on C set if that means
 * something, else what the call makes of its registers */
#define CALL(no, r)                          \
    uint64_t ret = swi((no) | X_BIT, (r));   \
    if (FAILED(ret))                         \
        return _kernel_ERROR

int _kernel_osbyte(int op, int x, int y)
{
    uint32_t r[10] = { (uint32_t)op, (uint32_t)x, (uint32_t)y };
    CALL(SWI_Byte, r);
    return (int)((r[1] & 0xFF) | (r[2] & 0xFF) << 8 | (CARRY(ret) ? 1u << 16 : 0));
}

int _kernel_osrdch(void)
{
    uint32_t r[10] = { 0 };
    CALL(SWI_ReadC, r);
    return CARRY(ret) ? -1 : (int)(r[0] & 0xFF);
}

int _kernel_oswrch(int ch)
{
    uint32_t r[10] = { (uint32_t)ch & 0xFF };
    CALL(SWI_WriteC, r);
    return 0;
}

int _kernel_osbget(unsigned handle)
{
    uint32_t r[10] = { 0, handle };
    CALL(SWI_BGet, r);
    return CARRY(ret) ? -1 : (int)(r[0] & 0xFF);
}

int _kernel_osbput(int ch, unsigned handle)
{
    uint32_t r[10] = { (uint32_t)ch & 0xFF, handle };
    CALL(SWI_BPut, r);
    return 0;
}

int _kernel_osgbpb(int op, unsigned handle, _kernel_osgbpb_block *b)
{
    uint32_t r[10] = { (uint32_t)op, handle, (uint32_t)(uintptr_t)b->dataptr, (uint32_t)b->nbytes,
                       (uint32_t)b->fileptr, (uint32_t)b->buf_len, (uint32_t)(uintptr_t)b->wild_fld };
    CALL(SWI_GBPB, r);
    b->dataptr = (void *)(uintptr_t)r[2];
    b->nbytes = (int)r[3];
    b->fileptr = (int)r[4];
    return CARRY(ret) ? -1 : 0;
}

int _kernel_osword(int op, int *data)
{
    uint32_t r[10] = { (uint32_t)op, (uint32_t)(uintptr_t)data };
    CALL(SWI_Word, r);
    return 0;
}

int _kernel_osfind(int op, char *name)
{
    uint32_t r[10] = { (uint32_t)op, (uint32_t)(uintptr_t)name };
    CALL(SWI_Find, r);
    return (int)r[0];
}

int _kernel_osfile(int op, const char *name, _kernel_osfile_block *b)
{
    uint32_t r[10] = { (uint32_t)op, (uint32_t)(uintptr_t)name, (uint32_t)b->load, (uint32_t)b->exec,
                       (uint32_t)b->start, (uint32_t)b->end };
    CALL(SWI_File, r);
    b->load = (int)r[2], b->exec = (int)r[3], b->start = (int)r[4], b->end = (int)r[5];
    return (int)r[0];
}

int _kernel_osargs(int op, unsigned handle, int arg)
{
    uint32_t r[10] = { (uint32_t)op, handle, (uint32_t)arg };
    CALL(SWI_Args, r);
    return op == 0 && handle == 0 ? (int)r[0] : (int)r[2];
}

int _kernel_oscli(const char *s)
{
    uint32_t r[10] = { (uint32_t)(uintptr_t)s };
    CALL(SWI_CLI, r);
    return 1;
}

_kernel_oserror *_kernel_getenv(const char *name, char *buffer, unsigned size)
{
    if (!size)
        return NULL;
    /* R4 3: the value as a string, whatever its type */
    uint32_t r[10] = { (uint32_t)(uintptr_t)name, (uint32_t)(uintptr_t)buffer, size - 1, 0, 3 };
    uint64_t ret = swi(SWI_ReadVarVal | X_BIT, r);
    if (FAILED(ret)) {
        buffer[0] = 0;
        return (_kernel_oserror *)(uintptr_t)(uint32_t)ret;
    }
    buffer[r[2] < size ? r[2] : size - 1] = 0;
    return NULL;
}

_kernel_oserror *_kernel_setenv(const char *name, const char *value)
{
    /* a string, or R2 negative: deleted */
    uint32_t r[10] = { (uint32_t)(uintptr_t)name, (uint32_t)(uintptr_t)value,
                       value ? (uint32_t)strlen(value) : (uint32_t)-1, 0, 0 };
    uint64_t ret = swi(SWI_SetVarVal | X_BIT, r);
    return FAILED(ret) ? (_kernel_oserror *)(uintptr_t)(uint32_t)ret : NULL;
}
