/*
 * Program entry and the raw system call interface for static arm64 programs.
 *
 * XNU arm64 convention: call number in x16, arguments in x0-x7, "svc #0x80".
 * On failure the carry flag is set and x0 holds the error number.
 */
	.text
	.align 2

/* The kernel enters here with sp pointing at argc, argv[], NULL, envp[], NULL. */
	.globl _start
_start:
	mov	x0, sp
	and	sp, x0, #0xfffffffffffffff0
	mov	x29, #0
	mov	x30, #0
	bl	___mini_start
	brk	#1

/* long mini_syscall(long number, long a, long b, long c, long d, long e, long f) */
	.globl _mini_syscall
_mini_syscall:
	mov	x16, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	mov	x3, x4
	mov	x4, x5
	mov	x5, x6
	svc	#0x80
	b.cs	Lerror
	ret

/* fork returns twice; the kernel marks the child with x1 = 1 */
	.globl _fork
_fork:
	mov	x16, #2
	svc	#0x80
	b.cs	Lerror
	cbz	x1, 1f
	mov	x0, #0
1:	ret

/* pipe returns the two descriptors in x0 and x1 */
	.globl _pipe
_pipe:
	mov	x9, x0
	mov	x16, #42
	svc	#0x80
	b.cs	Lerror
	str	w0, [x9]
	str	w1, [x9, #4]
	mov	x0, #0
	ret

Lerror:
	adrp	x9, _errno@PAGE
	str	w0, [x9, _errno@PAGEOFF]
	mov	x0, #-1
	ret
