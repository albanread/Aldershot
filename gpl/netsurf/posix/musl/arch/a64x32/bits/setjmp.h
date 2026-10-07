/* A64X32: what setjmp keeps is AArch64's -- x19-x30, sp, d8-d15 -- in
 * 64-bit words however wide a pointer is (ROSGD overlay) */
typedef unsigned long long __jmp_buf[22];
