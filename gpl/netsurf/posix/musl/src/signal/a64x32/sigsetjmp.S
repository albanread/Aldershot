// sigsetjmp -- A64X32 (ROSGD overlay, design 26).  Upstream's aarch64 code
// keeps x30 and x19 across setjmp in the sigjmp_buf's __fl and __ss[1];
// with ILP32's 4-byte __fl, __ss starts at 180 and __sigsetjmp_tail's mask
// (8 bytes there) would land on the saved x30.  They go further into __ss
// here, at 192 and 200, past the mask; and the address is zero-extended.
.global sigsetjmp
.global __sigsetjmp
.type sigsetjmp,%function
.type __sigsetjmp,%function
sigsetjmp:
__sigsetjmp:
	mov w0, w0
	cbz w1,setjmp

	str x30,[x0,#192]
	str x19,[x0,#200]
	mov x19,x0

	bl setjmp

	mov w1,w0
	mov x0,x19
	ldr x30,[x0,#192]
	ldr x19,[x0,#200]

.hidden __sigsetjmp_tail
	b __sigsetjmp_tail
.section .note.GNU-stack,"",%progbits
