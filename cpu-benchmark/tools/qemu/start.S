/*
 * start.S - process entry, system calls and VSCR access for the qemu-ppc
 * runner (Linux PPC32 user mode, big-endian, 32-bit SysV ABI).
 *
 * Entry state (Linux ELF ABI, static executable): r1 points at argc, followed
 * by argv[0..argc-1], NULL, envp[], NULL, auxv.  Nothing else is set up: there
 * is no libc, so _start builds the first stack frame itself, puts the AltiVec
 * unit into non-Java mode and calls qrt_main(argc, argv).  qrt_main's return
 * value is passed to exit_group.
 *
 * System calls: "sc" with the number in r0 and arguments in r3..r8.  The
 * result comes back in r3; CR0[SO] set means r3 holds a positive errno.
 */
	.section ".text"
	.align	2

	.globl	_start
	.type	_start, @function
_start:
	mr	14, 1			/* r14 -> argc (r14 is callee-saved) */
	clrrwi	1, 1, 4			/* 16-byte aligned stack */
	li	0, 0
	stwu	1, -64(1)		/* first frame, back chain = NULL */
	stw	0, 0(1)
	stw	0, 4(1)
	mtlr	0

	/* VSCR = NJ (non-Java mode: vector denormals flush to zero), the Linux
	 * default for user processes, and SAT = 0.  Built in an aligned slot. */
	lis	3, 1			/* 0x00010000 = VSCR[NJ] */
	stw	0, 32(1)
	stw	0, 36(1)
	stw	0, 40(1)
	stw	3, 44(1)
	addi	4, 1, 32
	lvx	0, 0, 4
	mtvscr	0
	vxor	0, 0, 0

	lwz	3, 0(14)		/* argc */
	addi	4, 14, 4		/* argv */
	bl	qrt_main
	b	qrt_exit		/* exit_group(r3), never returns */
	.size	_start, . - _start

/* long qrt_syscall(long nr, long a, long b, long c, long d)
 * Returns the result, or -errno on failure. */
	.globl	qrt_syscall
	.type	qrt_syscall, @function
qrt_syscall:
	mr	0, 3
	mr	3, 4
	mr	4, 5
	mr	5, 6
	mr	6, 7
	sc
	bnslr
	neg	3, 3
	blr
	.size	qrt_syscall, . - qrt_syscall

/* void qrt_exit(int status) - exit_group, falling back to exit. */
	.globl	qrt_exit
	.type	qrt_exit, @function
qrt_exit:
	mr	31, 3
	li	0, 234			/* exit_group */
	sc
	mr	3, 31
	li	0, 1			/* exit */
	sc
1:	b	1b
	.size	qrt_exit, . - qrt_exit

/* uint32_t qrt_get_vscr(void) */
	.globl	qrt_get_vscr
	.type	qrt_get_vscr, @function
qrt_get_vscr:
	stwu	1, -48(1)
	addi	4, 1, 16		/* 16-byte aligned: r1 is kept 16-aligned */
	clrrwi	4, 4, 4
	mfvscr	0
	stvx	0, 0, 4
	lwz	3, 12(4)		/* the VSCR is the last word */
	vxor	0, 0, 0
	addi	1, 1, 48
	blr
	.size	qrt_get_vscr, . - qrt_get_vscr

/* void qrt_set_vscr(uint32_t vscr) */
	.globl	qrt_set_vscr
	.type	qrt_set_vscr, @function
qrt_set_vscr:
	stwu	1, -48(1)
	addi	4, 1, 16
	clrrwi	4, 4, 4
	li	0, 0
	stw	0, 0(4)
	stw	0, 4(4)
	stw	0, 8(4)
	stw	3, 12(4)
	lvx	0, 0, 4
	mtvscr	0
	vxor	0, 0, 0
	addi	1, 1, 48
	blr
	.size	qrt_set_vscr, . - qrt_set_vscr

	.section .note.GNU-stack, "", @progbits
