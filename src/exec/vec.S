/* Program (0x700) and alignment (0x600) interrupt stubs, and the crashdump override.
 *
 * libxenon's own vectors save state and jump to crashdump(). While cases run we want an interrupt INSIDE the case (the
 * slot or the kernel buffer, or an extra range such as a sibling kernel) to be recorded and skipped, and every other
 * interrupt to stay fatal. Ranges: the slot, the kernel buffer, and XTS_EXEC_EXTRA_RANGES (4) extra ones
 * (xts_exec_trap_range, each < 64 KB). The stub (copied to exc_base+0x600 and +0x700 by src/exec/exec_console.c, which
 * patches the range bounds, the vector number and the branch to libxenon's ex_alignment/ex_program into the copy) uses libxenon's
 * per-thread HV context block (SPRG1 = its real address, SPRG0 = r31 scratch, exactly as DO_EXCEPT in except.S):
 *
 *   in range:  ctx[36] = SRR0, ctx[37] = SRR1, ctx[80] = vector, ctx[81] = DAR; resume at SRR0+4
 *   otherwise: restore r3/r4/CR and continue exactly like DO_EXCEPT (std r1..r4, li r3,vector, b ex_<handler>)
 *              -> libxenon dumps state -> crashdump (below) -> xts_crash_entry (C): crash record, never FAT.
 *
 * Interrupts run with MSR[SF]=1, translation off; only SPRs, GPRs and the context block (real address) are touched.
 * SRR0 is compared on its low 32 bits (cmplw), which is what a case address is in either mode.
 * The stub must stay below 0x100 bytes (vectors are 0x100 apart): checked at install time.
 */
#define CTX_SRR0   0x120   /* 8*36 */
#define CTX_SRR1   0x128   /* 8*37 */
#define CTX_VECTOR (8*80)
#define CTX_DAR    (8*81)
#define CTX_CR     (8*86)

/* [lo, hi) with two 32-bit compares (any size): the kernel buffer */
#define RANGE(n)                       \
	.globl xts_vec_r##n##_lo;      \
xts_vec_r##n##_lo:                     \
	lis 4, 0;                      \
	ori 4, 4, 0;                   \
	cmplw 3, 4;                    \
	blt 1f;                        \
	.globl xts_vec_r##n##_hi;      \
xts_vec_r##n##_hi:                     \
	lis 4, 0;                      \
	ori 4, 4, 0;                   \
	cmplw 3, 4;                    \
	blt xts_vec_in;                \
1:
/* (srr0 - lo) mod 2^32 < size, size < 64 KB (0 = empty): patched addis/addi = -lo, cmplwi = size. The slot and the
 * extra ranges. */
#define RANGE16(n)                     \
	.globl xts_vec_s##n;           \
xts_vec_s##n:                          \
	addis 4, 3, 0;                 \
	addi 4, 4, 0;                  \
	cmplwi 4, 0;                   \
	blt xts_vec_in;

	.section .text
	.globl xts_vec_stub
	.globl xts_vec_stub_end
	.globl xts_vec_li_out
	.globl xts_vec_b_out
	.globl xts_vec_li_in
	.align 4
xts_vec_stub:
	mtsprg0 31
	mfsprg1 31
	std 3, 0x18(31)
	std 4, 0x20(31)
	mfcr 3
	std 3, CTX_CR(31)
	mfsrr0 3
	clrldi 3, 3, 32
	RANGE16(0)              /* slot */
	RANGE(1)                /* kernel buffer */
	RANGE16(2)              /* extra ranges 0..3 */
	RANGE16(3)
	RANGE16(4)
	RANGE16(5)
	/* outside every range: become libxenon's DO_EXCEPT */
	ld 3, CTX_CR(31)
	mtcrf 255, 3
	ld 3, 0x18(31)
	ld 4, 0x20(31)
	std 1, 0x8(31)
	std 2, 0x10(31)
xts_vec_li_out:
	li 3, 0                 /* patched: li 3, <vector> */
xts_vec_b_out:
	b .                     /* patched: b ex_alignment / ex_program */
xts_vec_in:
	mfsrr0 3
	std 3, CTX_SRR0(31)
	addi 3, 3, 4
	mtsrr0 3
	mfsrr1 3
	std 3, CTX_SRR1(31)
	mfdar 3
	std 3, CTX_DAR(31)
xts_vec_li_in:
	li 3, 0                 /* patched: li 3, <vector> */
	std 3, CTX_VECTOR(31)
	ld 3, CTX_CR(31)
	mtcrf 255, 3
	ld 3, 0x18(31)
	ld 4, 0x20(31)
	mfsprg0 31
	rfid
xts_vec_stub_end:

/* libxenon's fault path ends with "r3 = vector, r4 = context EA; rfid to crashdump", entering with the MSR of the
 * interrupted code and ITS r1/r2/r13 (a fault inside a case or a kernel can have any r1). Defining `crashdump` here keeps
 * libxenon's own (display-based) version out of the link. Before any C:
 *   - clear MSR[SF] (this binary's C code is 32-bit);
 *   - count the entry in xts_crash_depth[pir]: 0 -> normal crash handling, 1 -> nested crash (the handler itself
 *     faulted: xts_crash_entry only draws a minimal screen line), >= 2 -> spin here without touching anything;
 *   - r1 = top of this PIR's emergency stack (xts_crash_stacks[pir], 16 KB = 1 << 14, src/plat/plat_console.c) - 64,
 *     back chain 0;
 *   - r2 / r13 = the values main() ran with (captured by xts_plat_init: libxenon's crt sets neither and this build uses
 *     no small-data base registers, so they are only restored for safety).
 * Then xts_crash_entry(r3 = vector, r4 = context, r5 = depth before this entry). PIR > 5 cannot happen; clamped to 5. */
	.globl crashdump
	.align 4
crashdump:
	mfmsr 5
	rldicl 5, 5, 0, 1
	mtmsrd 5
	isync
	mfspr 5, 1023
	clrldi 5, 5, 61
	cmplwi 5, 5
	ble 1f
	li 5, 5
1:	lis 6, xts_crash_depth@ha
	addi 6, 6, xts_crash_depth@l
	slwi 7, 5, 2
	lwzx 8, 6, 7
	addi 9, 8, 1
	stwx 9, 6, 7
	sync
	cmplwi 8, 2
	bge 2f
	lis 6, xts_crash_stacks@ha
	addi 6, 6, xts_crash_stacks@l
	addi 7, 5, 1
	slwi 7, 7, 14
	add 1, 6, 7
	clrldi 1, 1, 32
	addi 1, 1, -64
	li 0, 0
	stw 0, 0(1)
	lis 6, xts_crash_r2@ha
	lwz 2, xts_crash_r2@l(6)
	lis 6, xts_crash_r13@ha
	lwz 13, xts_crash_r13@l(6)
	mr 5, 8
	b xts_crash_entry
2:	b 2b
