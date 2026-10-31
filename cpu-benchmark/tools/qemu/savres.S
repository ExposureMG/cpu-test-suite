/*
 * savres.S - out-of-line register save/restore routines of the 32-bit SysV
 * PowerPC ABI.  GCC calls them at -Os (and sometimes with -O2 on large
 * frames); -O2 kernels normally do not need them.  Written here so the qemu
 * runner never links a toolchain's libgcc.a.  Same entry points and register
 * conventions as libgcc's crtsavgpr/crtresgpr/crtresxgpr/crtsavfpr/crtresfpr/
 * crtresxfpr/crtsavevr/crtrestvr (checked against the testxenon3 objects):
 *
 *   _savegpr_N / _restgpr_N      r11 = end of the GPR save area, stw/lwz rN..r31
 *   _restgpr_N_x                 ditto, then lr = 4(r11), r1 = r11, return
 *   _savefpr_N / _restfpr_N      r11 = end of the FPR save area, stfd/lfd fN..f31
 *   _restfpr_N_x                 ditto, then lr = 4(r11), r1 = r11, return
 *   _savevr_N / _restvr_N        r0 = end of the VR save area, r11 scratch
 *
 * The VMX128 variants (_savevr128_N, v64-v127) are not provided: the qemu
 * build uses -mno-vmx128.
 */
	.section ".text"
	.align	2

	.macro	FUNC name
	.globl	\name
	.type	\name, @function
\name:
	.endm

/* ---- GPRs ------------------------------------------------------------- */
	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	FUNC	_savegpr_\n
	stw	\n, ((\n) - 32) * 4(11)
	.endr
	blr

	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	FUNC	_restgpr_\n
	lwz	\n, ((\n) - 32) * 4(11)
	.endr
	blr

	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	FUNC	_restgpr_\n\()_x
	lwz	\n, ((\n) - 32) * 4(11)
	.endr
	FUNC	_restgpr_31_x
	lwz	0, 4(11)
	lwz	31, -4(11)
	mtlr	0
	mr	1, 11
	blr

/* ---- FPRs ------------------------------------------------------------- */
	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	FUNC	_savefpr_\n
	stfd	\n, ((\n) - 32) * 8(11)
	.endr
	blr

	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	FUNC	_restfpr_\n
	lfd	\n, ((\n) - 32) * 8(11)
	.endr
	blr

	.irp	n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	FUNC	_restfpr_\n\()_x
	lfd	\n, ((\n) - 32) * 8(11)
	.endr
	FUNC	_restfpr_31_x
	lwz	0, 4(11)
	lfd	31, -8(11)
	mtlr	0
	mr	1, 11
	blr

/* ---- AltiVec VRs (v20-v31 are callee-saved) --------------------------- */
	.irp	n, 20,21,22,23,24,25,26,27,28,29,30,31
	FUNC	_savevr_\n
	li	11, ((\n) - 32) * 16
	stvx	\n, 11, 0
	.endr
	blr

	.irp	n, 20,21,22,23,24,25,26,27,28,29,30,31
	FUNC	_restvr_\n
	li	11, ((\n) - 32) * 16
	lvx	\n, 11, 0
	.endr
	blr

	.section .note.GNU-stack, "", @progbits
