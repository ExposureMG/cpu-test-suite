/*
 * lib_mathf.c - LIB: the toolchain's newlib float math, sinf cosf expf logf
 * sqrtf, 2048 calls each.
 *
 * Calls go through BLIB(), so each variant runs the libm built by its own
 * toolchain (newlib's fdlibm-derived float routines, with -mcpu=xenon or the
 * cell default).  Inputs, uniform unless noted:
 *   sinf, cosf  [-100, 100]   (argument reduction for |x| > pi/4)
 *   expf        [-80, 80]
 *   logf        [2^-99, 2^99) ~ [1.6e-30, 6.3e29], log-uniform: a uniform
 *               binary exponent and a uniform 23-bit mantissa
 *   sqrtf       [0, 1e6]
 * No denormal inputs or results, no errno-setting domain errors.
 *
 * Result: a weighted sum of all 10240 results, chosen so that every result
 * matters.  A plain sum would be 1e35, set by the few largest expf values,
 * and would hide everything else.  So each expf result is scaled by a
 * precomputed 2^-trunc(x * log2(e)) (exact; the scaled values lie in
 * (0.5, 2)), and the logf and sqrtf sums are weighted by 2^-6 and 2^-10
 * (|log| < 69, sqrt <= 1000).  Every function then contributes values of
 * order one: sinf -53.9, cosf -30.3, expf 2240.4, logf 2.3, sqrtf 1341.6,
 * total 3500.06.
 *
 * The two libm builds share newlib's fdlibm float cores (the same number of
 * fmadds in every core routine) but not the wrappers around them:
 * test16old's newlib has the SVID/X-Open wrappers (every expf, logf and
 * sqrtf call loads __fdlib_version and has a matherr path), testxenon3's
 * the C99 ones (__math_oflowf, with_errnof, ...).  So this kernel measures
 * each toolchain's libm as shipped, wrappers included.
 *
 * APPROX, relative tolerance 1e-6.  On the normal path the wrappers only
 * decide how errors would be reported, so the results can differ only where
 * FMA contraction in the cores moves a result by an ulp; sqrtf is correctly
 * rounded in both.  All 8192
 * non-sqrt results off by 2 ulp in the same direction (weighted ulp
 * <= 1.2e-7) would move the total by 1.5e-3, i.e. 4e-7 relative, so 1e-6
 * cannot fail on rounding alone.  Measured with newlib 4.6.0's own sources
 * built for the host with and without contraction, on these inputs: 24 of
 * the 10240 results differ, by 1 ulp each, and the total by 2.6e-11.  1e-6
 * still catches any single result off by more than 3.5e-3 (weighted).
 */
#include "bench_lib.h"

#define NCALL 2048

#define ARR_BYTES ((uint32_t)(NCALL * sizeof(float))) /* 8 KB */
#define OFF_SIN  0u
#define OFF_COS  (OFF_SIN + ARR_BYTES)
#define OFF_EXP  (OFF_COS + ARR_BYTES)
#define OFF_ESCL (OFF_EXP + ARR_BYTES)
#define OFF_LOG  (OFF_ESCL + ARR_BYTES)
#define OFF_SQRT (OFF_LOG + ARR_BYTES)
#define ARENA_BYTES (OFF_SQRT + ARR_BYTES)

#define LOG2EF 1.44269504088896341f

typedef union fbits {
	float f;
	uint32_t u;
} fbits;

/* Uniform in [-scale, scale): 2 * u - 1 is exact, so one rounding. */
static float rand_sym(bench_rng *r, float scale)
{
	return scale * (2.0f * bench_rng_f01(r) - 1.0f);
}

static void mathf_init(void *arena)
{
	float *xs = bench_at(arena, OFF_SIN);
	float *xc = bench_at(arena, OFF_COS);
	float *xe = bench_at(arena, OFF_EXP);
	float *es = bench_at(arena, OFF_ESCL);
	float *xl = bench_at(arena, OFF_LOG);
	float *xq = bench_at(arena, OFF_SQRT);
	bench_rng r;
	int i;

	bench_rng_seed(&r, 0x6D746866u); /* "mthf" */
	for (i = 0; i < NCALL; i++) {
		fbits b;
		int e;

		xs[i] = rand_sym(&r, 100.0f);
		xc[i] = rand_sym(&r, 100.0f);
		xe[i] = rand_sym(&r, 80.0f);
		/* 2^-trunc(x * log2(e)): expf(x) * es[i] is in (0.5, 2) */
		b.u = (uint32_t)(127 - (int)(xe[i] * LOG2EF)) << 23;
		es[i] = b.f;
		/* 2^e * 1.m with e uniform in [-99, 98] */
		e = (int)bench_rng_below(&r, 198) - 99;
		b.u = ((uint32_t)(e + 127) << 23) | (bench_rng_next(&r) >> 9);
		xl[i] = b.f;
		xq[i] = 1e6f * bench_rng_f01(&r);
	}
}

static double mathf_run(void *arena)
{
	const float *xs = bench_at(arena, OFF_SIN);
	const float *xc = bench_at(arena, OFF_COS);
	const float *xe = bench_at(arena, OFF_EXP);
	const float *es = bench_at(arena, OFF_ESCL);
	const float *xl = bench_at(arena, OFF_LOG);
	const float *xq = bench_at(arena, OFF_SQRT);
	double ssin = 0.0, scos = 0.0, sexp = 0.0, slog = 0.0, ssqrt = 0.0;
	int i;

	for (i = 0; i < NCALL; i++)
		ssin += BLIB(sinf)(xs[i]);
	for (i = 0; i < NCALL; i++)
		scos += BLIB(cosf)(xc[i]);
	for (i = 0; i < NCALL; i++)
		sexp += BLIB(expf)(xe[i]) * es[i];
	for (i = 0; i < NCALL; i++)
		slog += BLIB(logf)(xl[i]);
	for (i = 0; i < NCALL; i++)
		ssqrt += BLIB(sqrtf)(xq[i]);
	return ssin + scos + sexp + slog * 0x1p-6 + ssqrt * 0x1p-10;
}

BENCH_KDEF(mathf) = {
	.name = "mathf", .desc = "newlib sinf cosf expf logf sqrtf", .unit = "call",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_APPROX, .tolerance = 1e-6,
	.units = 5 * NCALL, .arena_bytes = ARENA_BYTES,
	.init = mathf_init, .run = mathf_run,
};
