/*
 * fp_matmul.c - FP: 64x64 double matrix multiply, C = A * B.
 *
 * The textbook cache-friendly i-k-j order: for each row i of C, the inner
 * loop streams row k of B and row i of C with the scalar a[i][k] held in a
 * register, so it is a pure load / multiply-add / store stream.  Each C row
 * is initialised from the k = 0 term instead of being zeroed first (no
 * zeroing loop, so GCC has no memset to emit), then k = 1..63 accumulate.
 *
 * Work: 64^3 multiply-adds = 524288 flops.  The three matrices take 96 KB,
 * well inside L2; B (32 KB) is re-read for every row of C.
 *
 * Result: the sum of all of C, 54.5997.  APPROX, relative tolerance 1e-12.
 * Each C[i][j] is a 64-term dot product of values in [-1, 1) (|C| < 16).  A
 * compiler that contracts c += a * b into fmadd skips the product's rounding
 * (at most 2^-54, since |a * b| < 1), after which the accumulator's own
 * roundings (ulp <= 3.6e-15) can differ too.  As random walks that is
 * ~1.5e-15 per element, ~1e-13 over the 4096-element sum and ~2e-13 from
 * the sum's own re-rounding (ulp(54.6) = 7e-15): about 4e-15 relative.
 * Exceeding 1e-12 would take thousands of independent roundings all going
 * the same way.  Measured on the host: fmadd versus separate multiply and
 * add changes the sum by 1.8e-15 relative, and both Xenon compilers emit
 * the same fmadd / fmul sequence.  1e-12 still catches any element of C off
 * by more than 6e-11.
 */
#include "bench_kernel.h"

#define N 64u

#define MAT_BYTES ((uint32_t)(N * N * sizeof(double))) /* 32 KB */
#define OFF_A 0u
#define OFF_B (OFF_A + MAT_BYTES)
#define OFF_C (OFF_B + MAT_BYTES)
#define ARENA_BYTES (OFF_C + MAT_BYTES)

static void matmul_init(void *arena)
{
	double *a = bench_at(arena, OFF_A);
	double *b = bench_at(arena, OFF_B);
	bench_rng r;
	uint32_t i;

	/* Uniform in [-1, 1); 2 * u - 1 is exact for the 53-bit u. */
	bench_rng_seed(&r, 0x6D61746Du); /* "matm" */
	for (i = 0; i < N * N; i++)
		a[i] = 2.0 * bench_rng_d01(&r) - 1.0;
	for (i = 0; i < N * N; i++)
		b[i] = 2.0 * bench_rng_d01(&r) - 1.0;
}

/* c = a * b for row-major N x N matrices, i-k-j order. */
static void mat_mul(double *restrict c, const double *restrict a,
		    const double *restrict b)
{
	uint32_t i, j, k;

	for (i = 0; i < N; i++) {
		double *restrict ci = c + i * N;
		const double *ai = a + i * N;
		const double a0 = ai[0];

		for (j = 0; j < N; j++)
			ci[j] = a0 * b[j];
		for (k = 1; k < N; k++) {
			const double aik = ai[k];
			const double *bk = b + k * N;

			for (j = 0; j < N; j++)
				ci[j] += aik * bk[j];
		}
	}
}

static double matmul_run(void *arena)
{
	const double *a = bench_at(arena, OFF_A);
	const double *b = bench_at(arena, OFF_B);
	double *c = bench_at(arena, OFF_C);
	double sum = 0.0;
	uint32_t i;

	mat_mul(c, a, b);
	for (i = 0; i < N * N; i++)
		sum += c[i];
	return sum;
}

BENCH_KDEF(matmul) = {
	.name = "matmul", .desc = "64x64 double C=A*B, i-k-j", .unit = "flop",
	.category = BENCH_CAT_FP, .flags = BENCH_F_APPROX, .tolerance = 1e-12,
	.units = 2u * N * N * N, .arena_bytes = ARENA_BYTES,
	.init = matmul_init, .run = matmul_run,
};
