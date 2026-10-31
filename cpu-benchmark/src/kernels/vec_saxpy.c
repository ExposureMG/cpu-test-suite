/*
 * vec_saxpy.c - VEC: plain-C BLAS level 1 for the auto-vectoriser.
 *
 * z[i] = a * x[i] + y[i] over 16384 floats (saxpy), then the dot product of
 * x and y with four explicit partial sums s0..s3 in a stride-4 loop (the
 * classic hand-unrolled sdot: one SLP group of 4 independent reductions, so
 * it can be vectorised without reassociating anything).  No intrinsics: the
 * point is to see what each compiler's vectoriser does at -O2 with restrict
 * pointers and 16-byte-aligned arrays (told to the compiler with
 * __builtin_assume_aligned, as a SIMD-aware engine would), and how it does
 * the FP multiply-adds (fmadds vs vmaddfp, contraction).
 *
 * The scale a is read from the arena so it is not a compile-time constant.
 * x and y are uniform in [0, 1) (24-bit mantissas), a = 1.25.
 *
 * Result: dot(x, y) + the sum of every 64th z (256 samples), about
 * 4.1e3 + 2.9e2 = 4.39e3.  APPROX, relative tolerance 1e-6.  Both compilers
 * contract a * x + y and s += x * y to fused multiply-adds by default (gnu11:
 * -ffp-contract=fast), a vector vmaddfp rounds exactly like a scalar fmadds
 * (no denormals here, so AltiVec's non-IEEE mode does not matter), and the
 * SLP-vectorised reduction keeps the lane order of s0..s3.  So A and B
 * should agree bit for bit (they do under qemu-ppc).  The legitimate
 * difference is fused vs unfused (one compiler not contracting): the
 * unfused product rounding (<= 1.5e-8 absolute) changes a partial sum only
 * when it pushes s + x * y across a rounding boundary of s (ulp 6e-5..1.2e-4
 * near 1e3), i.e. on a few of the 4096 additions per partial sum, by an ulp
 * each; each z sample moves by <= 1 ulp (<= 2.4e-7, 256 samples).
 * Simulated (float32, this data distribution, 40 seeds): <= 1.2e-7 relative
 * for the dot; measured on the host (x86 FMA vs -ffp-contract=off):
 * 1.2e-10 for the whole result.  1e-6 is ~8x the worst case; a real error
 * (a wrong or missing element: >= ~0.05 in the dot, a wrong z sample: ~1)
 * exceeds it (1e-6 * 4.39e3 = 0.0044).
 */
#include "bench_kernel.h"

#define N 16384u
#define VEC_BYTES (N * 4u) /* 64 KB */
#define OFF_PARAM 0u       /* a (one float, padded to a line) */
#define OFF_X 128u
#define OFF_Y (OFF_X + VEC_BYTES)
#define OFF_Z (OFF_Y + VEC_BYTES)
#define ARENA_BYTES (OFF_Z + VEC_BYTES)
#define Z_SAMPLE 64u

static void saxpy_init(void *arena)
{
	float *param = bench_at(arena, OFF_PARAM);
	float *x = bench_at(arena, OFF_X);
	float *y = bench_at(arena, OFF_Y);
	bench_rng r;
	uint32_t i;

	param[0] = 1.25f;
	bench_rng_seed(&r, 0x73617870u); /* "saxp" */
	for (i = 0; i < N; i++)
		x[i] = bench_rng_f01(&r);
	for (i = 0; i < N; i++)
		y[i] = bench_rng_f01(&r);
}

/* z = a * x + y */
static void saxpy(uint32_t n, float a, const float *restrict x,
		  const float *restrict y, float *restrict z)
{
	uint32_t i;

	x = __builtin_assume_aligned(x, 16);
	y = __builtin_assume_aligned(y, 16);
	z = __builtin_assume_aligned(z, 16);
	for (i = 0; i < n; i++)
		z[i] = a * x[i] + y[i];
}

/* x . y with 4 partial sums; n is a multiple of 4. */
static float sdot4(uint32_t n, const float *restrict x, const float *restrict y)
{
	float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
	uint32_t i;

	x = __builtin_assume_aligned(x, 16);
	y = __builtin_assume_aligned(y, 16);
	for (i = 0; i < n; i += 4) {
		s0 += x[i + 0] * y[i + 0];
		s1 += x[i + 1] * y[i + 1];
		s2 += x[i + 2] * y[i + 2];
		s3 += x[i + 3] * y[i + 3];
	}
	return (s0 + s1) + (s2 + s3);
}

static double saxpy_run(void *arena)
{
	const float a = *(const float *)bench_at(arena, OFF_PARAM);
	const float *x = bench_at(arena, OFF_X);
	const float *y = bench_at(arena, OFF_Y);
	float *z = bench_at(arena, OFF_Z);
	double r;
	uint32_t i;

	saxpy(N, a, x, y, z);
	r = sdot4(N, x, y);
	for (i = 0; i < N; i += Z_SAMPLE)
		r += z[i];
	return r;
}

BENCH_KDEF(saxpy) = {
	.name = "saxpy", .desc = "saxpy + 4-lane sdot, 16K floats", .unit = "elem",
	.category = BENCH_CAT_VEC, .flags = BENCH_F_APPROX, .tolerance = 1e-6,
	.units = N, .arena_bytes = ARENA_BYTES,
	.init = saxpy_init, .run = saxpy_run,
};
