/*
 * lib_mathd.c - LIB: the toolchain's newlib double math, sin cos exp log
 * sqrt, 2048 calls each.
 *
 * Calls go through BLIB(), so each variant runs the libm built by its own
 * toolchain (newlib's fdlibm routines, with -mcpu=xenon or the cell
 * default).  Inputs, uniform unless noted:
 *   sin, cos  [-100, 100]   (medium-size argument reduction)
 *   exp       [-700, 700]
 *   log       [2^-996, 2^996) ~ [1.5e-300, 6.7e299], log-uniform: a
 *             uniform binary exponent and a uniform 52-bit mantissa
 *   sqrt      [0, 1e6]
 * No denormal inputs or results, no errno-setting domain errors.
 *
 * Result: a weighted sum of all 10240 results, chosen so that every result
 * matters.  A plain sum would be 1e303, set by the few largest exp values,
 * and would hide everything else.  So each exp result is scaled by a
 * precomputed 2^-trunc(x * log2(e)) (exact; the scaled values lie in
 * (0.5, 2)), and the log and sqrt sums are weighted by 2^-9 and 2^-10
 * (|log| < 691, sqrt <= 1000).  Every function then contributes values of
 * order one: sin -39.2, cos -16.2, exp 2221.5, log -29.0, sqrt 1340.1,
 * total 3477.26.
 *
 * The two libm builds share newlib's fdlibm cores (the same number of
 * fmadds in every core routine) but not the wrappers around them:
 * test16old's newlib has the SVID/X-Open wrappers (every exp, log and sqrt
 * call loads __fdlib_version and has a matherr path), testxenon3's the C99
 * ones (__math_oflow, with_errno, ...).  So this kernel measures each
 * toolchain's libm as shipped, wrappers included.
 *
 * APPROX, relative tolerance 1e-12.  On the normal path the wrappers only
 * decide how errors would be reported, so the results can differ only where
 * FMA contraction in the cores moves a result by an ulp; sqrt is correctly
 * rounded in both.  All 8192
 * non-sqrt results off by 2 ulp in the same direction (weighted ulp
 * <= 2.2e-16) would move the total by 3.6e-12 (1e-15 relative).  Once an
 * accumulator differs, its later roundings can differ too: the exp sum
 * (ulp 4.5e-13) dominates, a random walk of ~1e-11 over its 2048 additions
 * and 1e-9 (3e-13 relative) if every one of them rounded the other way.
 * Measured with newlib 4.6.0's own sources built for the host with and
 * without contraction, on these inputs: 27 sin/cos results differ by 1 ulp
 * and the total is bit-identical.  1e-12 still catches any single result
 * off by more than 3.5e-9 (weighted).
 */
#include "bench_lib.h"

#define NCALL 2048

#define ARR_BYTES ((uint32_t)(NCALL * sizeof(double))) /* 16 KB */
#define OFF_SIN  0u
#define OFF_COS  (OFF_SIN + ARR_BYTES)
#define OFF_EXP  (OFF_COS + ARR_BYTES)
#define OFF_ESCL (OFF_EXP + ARR_BYTES)
#define OFF_LOG  (OFF_ESCL + ARR_BYTES)
#define OFF_SQRT (OFF_LOG + ARR_BYTES)
#define ARENA_BYTES (OFF_SQRT + ARR_BYTES)

#define LOG2E 1.44269504088896340736

typedef union dbits {
	double d;
	uint64_t u;
} dbits;

/* Uniform in [-scale, scale): 2 * u - 1 is exact, so one rounding. */
static double rand_sym(bench_rng *r, double scale)
{
	return scale * (2.0 * bench_rng_d01(r) - 1.0);
}

static void mathd_init(void *arena)
{
	double *xs = bench_at(arena, OFF_SIN);
	double *xc = bench_at(arena, OFF_COS);
	double *xe = bench_at(arena, OFF_EXP);
	double *es = bench_at(arena, OFF_ESCL);
	double *xl = bench_at(arena, OFF_LOG);
	double *xq = bench_at(arena, OFF_SQRT);
	bench_rng r;
	int i;

	bench_rng_seed(&r, 0x6D746864u); /* "mthd" */
	for (i = 0; i < NCALL; i++) {
		dbits b;
		uint64_t m;
		int e;

		xs[i] = rand_sym(&r, 100.0);
		xc[i] = rand_sym(&r, 100.0);
		xe[i] = rand_sym(&r, 700.0);
		/* 2^-trunc(x * log2(e)): exp(x) * es[i] is in (0.5, 2) */
		b.u = (uint64_t)(1023 - (int)(xe[i] * LOG2E)) << 52;
		es[i] = b.d;
		/* 2^e * 1.m with e uniform in [-996, 995] */
		e = (int)bench_rng_below(&r, 1992) - 996;
		m = (uint64_t)(bench_rng_next(&r) >> 12) << 32;
		m |= bench_rng_next(&r);
		b.u = ((uint64_t)(e + 1023) << 52) | m;
		xl[i] = b.d;
		xq[i] = 1e6 * bench_rng_d01(&r);
	}
}

static double mathd_run(void *arena)
{
	const double *xs = bench_at(arena, OFF_SIN);
	const double *xc = bench_at(arena, OFF_COS);
	const double *xe = bench_at(arena, OFF_EXP);
	const double *es = bench_at(arena, OFF_ESCL);
	const double *xl = bench_at(arena, OFF_LOG);
	const double *xq = bench_at(arena, OFF_SQRT);
	double ssin = 0.0, scos = 0.0, sexp = 0.0, slog = 0.0, ssqrt = 0.0;
	int i;

	for (i = 0; i < NCALL; i++)
		ssin += BLIB(sin)(xs[i]);
	for (i = 0; i < NCALL; i++)
		scos += BLIB(cos)(xc[i]);
	for (i = 0; i < NCALL; i++)
		sexp += BLIB(exp)(xe[i]) * es[i];
	for (i = 0; i < NCALL; i++)
		slog += BLIB(log)(xl[i]);
	for (i = 0; i < NCALL; i++)
		ssqrt += BLIB(sqrt)(xq[i]);
	return ssin + scos + sexp + slog * 0x1p-9 + ssqrt * 0x1p-10;
}

BENCH_KDEF(mathd) = {
	.name = "mathd", .desc = "newlib sin cos exp log sqrt", .unit = "call",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_APPROX, .tolerance = 1e-12,
	.units = 5 * NCALL, .arena_bytes = ARENA_BYTES,
	.init = mathd_init, .run = mathd_run,
};
