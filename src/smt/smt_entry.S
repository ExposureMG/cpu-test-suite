/* src/smt: run a JIT-built load kernel on a secondary hardware thread (console only; .S files are not built for the host).
 *
 *   void xts_smt_call(xts_smt_tcb_t *t, uint32_t code)       (r3 = tcb, r4 = code; called from the C worker)
 *
 * The worker runs inside libxenon's `secondary` loop task call (32-bit mode, translation on, FP and VMX available,
 * external interrupts off). The kernel owns every register; it is entered with r3 = tcb and ends with a branch (patched
 * in by smtb_install) to xts_smt_exit, which finds this thread's tcb again without trusting any GPR, restores the C
 * callee-saved state (r1 r2 r13 r14-r31 f14-f31 v20-v31 CR VRSAVE FPSCR VSCR, LR) and returns to the C worker.
 * Layout constants: smt_int.h.
 *
 * Exit self-check (a hardware crash was once reported with SRR0/LR near xts_smt_exit and DAR 0x3C(0) - consistent with
 * a NULL tcb in the restore sequence): the tcb is found TWICE and must agree before anything is loaded from it -
 *   SPRG2 (set by xts_smt_call; per thread, unused by libxenon, and load kernels never touch SPRs) and
 *   xts_smt_tcbp[PIR] (the table thread 0 filled)
 * - and the tcb must hold its own address (TCB_SV_SELF) and the PIR that entered (TCB_SV_PIR). On any mismatch nothing
 * is restored: exit_why = TCB_EXIT_BAD is stored into the table's tcb (if there is one) and the thread parks itself in a
 * low-priority loop; the C side then times out waiting for DONE and marks the thread lost (logged), instead of the
 * thread jumping through a garbage LR. xts_smt_call itself refuses a NULL tcb or code address (returns at once).
 */
#include "smt_int.h"

	.section .text
	.globl xts_smt_call
	.globl xts_smt_exit
	.align 7
xts_smt_call:
	cmplwi 3, 0
	beqlr
	cmplwi 4, 0
	beqlr
	mflr 0
	stw 0, TCB_SV_LR(3)
	stw 3, TCB_SV_SELF(3)
	mfspr 0, 1023
	clrldi 0, 0, 61
	stw 0, TCB_SV_PIR(3)
	mtsprg2 3
	stw 1, TCB_SV_R1(3)
	stw 2, TCB_SV_R2(3)
	stw 13, TCB_SV_R13(3)
	mfcr 0
	stw 0, TCB_SV_CR(3)
	mfspr 0, 256
	stw 0, TCB_SV_VRSAVE(3)
	std 14, TCB_SV_GPR+0(3)
	std 15, TCB_SV_GPR+8(3)
	std 16, TCB_SV_GPR+16(3)
	std 17, TCB_SV_GPR+24(3)
	std 18, TCB_SV_GPR+32(3)
	std 19, TCB_SV_GPR+40(3)
	std 20, TCB_SV_GPR+48(3)
	std 21, TCB_SV_GPR+56(3)
	std 22, TCB_SV_GPR+64(3)
	std 23, TCB_SV_GPR+72(3)
	std 24, TCB_SV_GPR+80(3)
	std 25, TCB_SV_GPR+88(3)
	std 26, TCB_SV_GPR+96(3)
	std 27, TCB_SV_GPR+104(3)
	std 28, TCB_SV_GPR+112(3)
	std 29, TCB_SV_GPR+120(3)
	std 30, TCB_SV_GPR+128(3)
	std 31, TCB_SV_GPR+136(3)
	stfd 14, TCB_SV_FPR+0(3)
	stfd 15, TCB_SV_FPR+8(3)
	stfd 16, TCB_SV_FPR+16(3)
	stfd 17, TCB_SV_FPR+24(3)
	stfd 18, TCB_SV_FPR+32(3)
	stfd 19, TCB_SV_FPR+40(3)
	stfd 20, TCB_SV_FPR+48(3)
	stfd 21, TCB_SV_FPR+56(3)
	stfd 22, TCB_SV_FPR+64(3)
	stfd 23, TCB_SV_FPR+72(3)
	stfd 24, TCB_SV_FPR+80(3)
	stfd 25, TCB_SV_FPR+88(3)
	stfd 26, TCB_SV_FPR+96(3)
	stfd 27, TCB_SV_FPR+104(3)
	stfd 28, TCB_SV_FPR+112(3)
	stfd 29, TCB_SV_FPR+120(3)
	stfd 30, TCB_SV_FPR+128(3)
	stfd 31, TCB_SV_FPR+136(3)
	mffs 0
	stfd 0, TCB_SV_FPSCR(3)
	li 5, TCB_SV_VR+0
	stvx 20, 3, 5
	li 5, TCB_SV_VR+16
	stvx 21, 3, 5
	li 5, TCB_SV_VR+32
	stvx 22, 3, 5
	li 5, TCB_SV_VR+48
	stvx 23, 3, 5
	li 5, TCB_SV_VR+64
	stvx 24, 3, 5
	li 5, TCB_SV_VR+80
	stvx 25, 3, 5
	li 5, TCB_SV_VR+96
	stvx 26, 3, 5
	li 5, TCB_SV_VR+112
	stvx 27, 3, 5
	li 5, TCB_SV_VR+128
	stvx 28, 3, 5
	li 5, TCB_SV_VR+144
	stvx 29, 3, 5
	li 5, TCB_SV_VR+160
	stvx 30, 3, 5
	li 5, TCB_SV_VR+176
	stvx 31, 3, 5
	mfvscr 0
	li 5, TCB_SV_VSCR
	stvx 0, 3, 5
	sync
	isync				/* the kernel was just written by thread 0 (xts_sync_code did dcbst/icbi) */
	mtctr 4
	bctr

	.align 5
xts_smt_exit:
	mfspr 4, 1023			/* PIR */
	clrldi 4, 4, 61			/* & 7 */
	cmplwi 4, 5
	bgt 9f
	slwi 5, 4, 2
	lis 6, xts_smt_tcbp@ha
	addi 6, 6, xts_smt_tcbp@l
	lwzx 6, 6, 5			/* r6 = tcb from the table */
	mfsprg2 3			/* r3 = tcb from SPRG2 */
	clrldi 3, 3, 32
	cmplwi 6, 0
	beq 9f
	cmplw 3, 6
	bne 8f
	lwz 5, TCB_SV_SELF(3)
	cmplw 5, 3
	bne 8f
	lwz 5, TCB_SV_PIR(3)
	cmplw 5, 4
	bne 8f
	li 5, TCB_SV_VSCR
	lvx 0, 3, 5
	mtvscr 0
	li 5, TCB_SV_VR+0
	lvx 20, 3, 5
	li 5, TCB_SV_VR+16
	lvx 21, 3, 5
	li 5, TCB_SV_VR+32
	lvx 22, 3, 5
	li 5, TCB_SV_VR+48
	lvx 23, 3, 5
	li 5, TCB_SV_VR+64
	lvx 24, 3, 5
	li 5, TCB_SV_VR+80
	lvx 25, 3, 5
	li 5, TCB_SV_VR+96
	lvx 26, 3, 5
	li 5, TCB_SV_VR+112
	lvx 27, 3, 5
	li 5, TCB_SV_VR+128
	lvx 28, 3, 5
	li 5, TCB_SV_VR+144
	lvx 29, 3, 5
	li 5, TCB_SV_VR+160
	lvx 30, 3, 5
	li 5, TCB_SV_VR+176
	lvx 31, 3, 5
	lfd 0, TCB_SV_FPSCR(3)
	mtfsf 0xff, 0
	lfd 14, TCB_SV_FPR+0(3)
	lfd 15, TCB_SV_FPR+8(3)
	lfd 16, TCB_SV_FPR+16(3)
	lfd 17, TCB_SV_FPR+24(3)
	lfd 18, TCB_SV_FPR+32(3)
	lfd 19, TCB_SV_FPR+40(3)
	lfd 20, TCB_SV_FPR+48(3)
	lfd 21, TCB_SV_FPR+56(3)
	lfd 22, TCB_SV_FPR+64(3)
	lfd 23, TCB_SV_FPR+72(3)
	lfd 24, TCB_SV_FPR+80(3)
	lfd 25, TCB_SV_FPR+88(3)
	lfd 26, TCB_SV_FPR+96(3)
	lfd 27, TCB_SV_FPR+104(3)
	lfd 28, TCB_SV_FPR+112(3)
	lfd 29, TCB_SV_FPR+120(3)
	lfd 30, TCB_SV_FPR+128(3)
	lfd 31, TCB_SV_FPR+136(3)
	ld 14, TCB_SV_GPR+0(3)
	ld 15, TCB_SV_GPR+8(3)
	ld 16, TCB_SV_GPR+16(3)
	ld 17, TCB_SV_GPR+24(3)
	ld 18, TCB_SV_GPR+32(3)
	ld 19, TCB_SV_GPR+40(3)
	ld 20, TCB_SV_GPR+48(3)
	ld 21, TCB_SV_GPR+56(3)
	ld 22, TCB_SV_GPR+64(3)
	ld 23, TCB_SV_GPR+72(3)
	ld 24, TCB_SV_GPR+80(3)
	ld 25, TCB_SV_GPR+88(3)
	ld 26, TCB_SV_GPR+96(3)
	ld 27, TCB_SV_GPR+104(3)
	ld 28, TCB_SV_GPR+112(3)
	ld 29, TCB_SV_GPR+120(3)
	ld 30, TCB_SV_GPR+128(3)
	ld 31, TCB_SV_GPR+136(3)
	lwz 0, TCB_SV_VRSAVE(3)
	mtspr 256, 0
	lwz 0, TCB_SV_CR(3)
	mtcrf 0xff, 0
	lwz 1, TCB_SV_R1(3)
	lwz 2, TCB_SV_R2(3)
	lwz 13, TCB_SV_R13(3)
	lwz 0, TCB_SV_LR(3)
	mtlr 0
	or 2, 2, 2			/* medium priority again (a same_insn word may have been a priority nop) */
	blr
8:	li 5, TCB_EXIT_BAD		/* inconsistent: tell the table's tcb, restore nothing */
	stw 5, TCB_EXITWHY(6)
	sync
9:	or 1, 1, 1			/* park this thread (the C side times out and marks it lost) */
	b 9b
