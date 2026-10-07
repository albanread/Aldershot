/* kernel.h -- RISC OS's C interface to the OS, for POSIX applications
 * (design 22, U-g).
 *
 * The part of SharedCLibrary's <kernel.h> a Unix port reaches for, with its
 * types and meanings: SWIs by register block, the OS_Byte family, the
 * command line, system variables and the last error.  ROSGD's own, written
 * from the interface's documentation (posix/riscos/kernel.c); what a POSIX
 * program has no use for -- stack chunks, the language support, the
 * library's own initialisation -- is not declared, so a port that names it
 * fails to compile rather than to run.
 *
 * A SWI goes through the C applications' gate (capp.h), R0-R9 in and out.
 * Addresses are the program's own: a POSIX program is x32, so a pointer is
 * the 32-bit address RISC OS sees.
 */
#ifndef __kernel_h
#define __kernel_h

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int r[10];                  /* R0-R9 */
} _kernel_swi_regs;

typedef struct {
    int load, exec;             /* load and execution addresses */
    int start, end;             /* start address or length, end or attributes */
} _kernel_osfile_block;

typedef struct {
    void *dataptr;              /* the data's address */
    int nbytes, fileptr;
    int buf_len;                /* the directory-reading calls' */
    char *wild_fld;             /* the name to match, wildcarded */
} _kernel_osgbpb_block;

typedef struct {
    int errnum;
    char errmess[252];          /* zero-terminated */
} _kernel_oserror;

/* _kernel_hostos's answers: RISC OS is Arthur's line */
#define _kernel_HOST_UNDEFINED (-1)
#define _kernel_ARTHUR         6
#define _kernel_A_UNIX         8

/* The host: _kernel_ARTHUR */
extern int _kernel_hostos(void);

/* Any SWI, with the X bit set unless _kernel_NONX is ORed into no; NULL, or
 * the error.  A non-X SWI's error goes to the program's error handler. */
#define _kernel_NONX 0x80000000
extern _kernel_oserror *_kernel_swi(int no, _kernel_swi_regs *in, _kernel_swi_regs *out);
/* The same, *carry set to the C flag the SWI returned */
extern _kernel_oserror *_kernel_swi_c(int no, _kernel_swi_regs *in, _kernel_swi_regs *out, int *carry);

/* The command line the program was run with (OS_GetEnv) */
extern char *_kernel_command_string(void);

/* The int calls below return >= 0 on success, -1 when the call returned C
 * set with no error (the end of a file, Escape), or _kernel_ERROR when it
 * failed: _kernel_last_oserror says why. */
#define _kernel_ERROR (-2)

/* OS_Byte: R1 in bits 0-7, R2 in bits 8-15, and bit 16 if C was set */
extern int _kernel_osbyte(int op, int x, int y);
extern int _kernel_osrdch(void);
extern int _kernel_oswrch(int ch);
extern int _kernel_osbget(unsigned handle);
extern int _kernel_osbput(int ch, unsigned handle);
extern int _kernel_osgbpb(int op, unsigned handle, _kernel_osgbpb_block *inout);
extern int _kernel_osword(int op, int *data);
/* OS_Find: an open returns the handle, 0 if the file was not found */
extern int _kernel_osfind(int op, char *name);
/* OS_File, R2-R5 from and back to the block; the result is R0 */
extern int _kernel_osfile(int op, const char *name, _kernel_osfile_block *inout);
/* OS_Args: the filing system's number for op = handle = 0, else R2 */
extern int _kernel_osargs(int op, unsigned handle, int arg);
/* OS_CLI */
extern int _kernel_oscli(const char *s);

/* The last error from any of these calls, or from _swix, since the last
 * call of this; NULL if there was none */
extern _kernel_oserror *_kernel_last_oserror(void);

/* System variables: OS_ReadVarVal into buffer (zero-terminated), and
 * OS_SetVarVal as a string (value NULL deletes the variable) */
extern _kernel_oserror *_kernel_getenv(const char *name, char *buffer, unsigned size);
extern _kernel_oserror *_kernel_setenv(const char *name, const char *value);

#ifdef __cplusplus
}
#endif

#endif
