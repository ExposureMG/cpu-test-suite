/*
 * fp_mathapx.c - FP: game-math-library sinf / cosf / expf approximations.
 *
 * The Cephes-derived single-precision routines that console engines carried
 * in their own math library: sin / cos reduce |x| by pi/4 with a float->int
 * conversion j = (int)(|x| * 4/pi), round j up to even (j += j & 1) so that
 * the reduced argument is in [-pi/4, pi/4], subtract j * pi/4 in three
 * Cody-Waite steps, pick the sine or cosine minimax polynomial and the sign
 * from bit tests on j, and evaluate the polynomial with Horner's rule.  expf
 * computes n = (int)(x * log2(e) +/- 0.5), reduces by n * ln 2 (two
 * Cody-Waite steps), evaluates a degree-7 polynomial and scales it by 2^n
 * assembled in the exponent bits of a float through a union.  So the kernel
 * is float<->int conversions, bit tests, FP compares and selects, and
 * dependent fmadds chains: exactly where code generation for Xenon differs.
 *
 * Inputs (8192 each, uniform): sin and cos in [-100, 100], exp in [-80, 80].
 * No range checks or special cases beyond what these inputs need, as in
 * most game math libraries (expf is valid for |x| < 87).
 *
 * Result: sum of all sin, all cos and all exp results, each exp result
 * scaled by 2^-k with k = trunc(x * log2(e)) precomputed per input (an exact
 * power-of-two scaling, so every exp evaluation contributes a value in
 * (0.5, 2) instead of the sum being 5e35 dominated by the largest inputs).
 * The sum is 8881.68 (the scaled exp results average 1.08).  APPROX,
 * relative tolerance 1e-6.  The octant j = (int)(|x| * 4/pi) and the first
 * two Cody-Waite products (DP1, DP2 have few significant bits) are exact
 * under any contraction, so the compilers cannot pick different octants;
 * what can differ is fmadds contraction of the last reduction step, of the
 * Horner steps and of the exp argument (which may move n by one at a
 * rounding boundary, giving an equally accurate result).  Each result then
 * differs by at most ~3 ulp (at most 3.6e-7 absolute for |value| < 2); all
 * 24576 of them off by 3 ulp in the same direction would move the sum by
 * 6e-3, i.e. 7e-7 relative, so 1e-6 cannot fail on rounding alone.  In
 * practice most results are identical: measured on the host, contracting
 * versus not contracting changes the sum by 1.1e-11.  1e-6 still catches a
 * single result off by more than 1e-2, and any wrong octant, sign or
 * coefficient.
 */
#include "bench_kernel.h"

#define NEVAL 8192

#define ARR_BYTES ((uint32_t)(NEVAL * sizeof(float))) /* 32 KB */
#define OFF_SIN  0u
#define OFF_COS  (OFF_SIN + ARR_BYTES)
#define OFF_EXP  (OFF_COS + ARR_BYTES)
#define OFF_ESCL (OFF_EXP + ARR_BYTES)
#define ARENA_BYTES (OFF_ESCL + ARR_BYTES)

typedef union fbits {
	float f;
	uint32_t u;
} fbits;

#define FOPI 1.27323954473516f /* 4 / pi */
/* pi/4 = DP1 + DP2 + DP3; DP1 and DP2 have 8 and 11 significant bits, so
 * j * DP1 and j * DP2 are exact for j < 2^13 (|x| < 6434). */
#define DP1 0.78515625f
#define DP2 2.4187564849853515625e-4f
#define DP3 3.77489497744594108e-8f

#define LOG2EF 1.44269504088896341f
/* ln 2 = LN2HI + LN2LO; n * LN2HI is exact for |n| < 2^15. */
#define LN2HI 0.693359375f
#define LN2LO -2.12194440e-4f

/* |x| -> r in [-pi/4, pi/4] and the even octant j: |x| = j * pi/4 + r. */
static inline float sc_reduce(float ax, int *octant)
{
	int j = (int)(ax * FOPI);
	float y;

	j += j & 1; /* map zeros to the origin: round j up to even */
	y = (float)j;
	*octant = j;
	return ((ax - y * DP1) - y * DP2) - y * DP3;
}

/* sin(r) on [-pi/4, pi/4], z = r * r */
static inline float sin_poly(float r, float z)
{
	return ((-1.9515295891e-4f * z + 8.3321608736e-3f) * z -
		1.6666654611e-1f) * z * r + r;
}

/* cos(r) on [-pi/4, pi/4], z = r * r */
static inline float cos_poly(float z)
{
	return ((2.443315711809948e-5f * z - 1.388731625493765e-3f) * z +
		4.166664568298827e-2f) * z * z - 0.5f * z + 1.0f;
}

static float apx_sinf(float x)
{
	int j, neg = 0;
	float r, z, s;

	if (x < 0.0f) { /* sin(-x) = -sin(x) */
		x = -x;
		neg = 4;
	}
	r = sc_reduce(x, &j);
	z = r * r;
	/* octants 2, 6: cosine; octants 4, 6: negative half-period */
	s = (j & 2) ? cos_poly(z) : sin_poly(r, z);
	return ((j ^ neg) & 4) ? -s : s;
}

static float apx_cosf(float x)
{
	int j;
	float r, z, c;

	if (x < 0.0f) /* even function */
		x = -x;
	r = sc_reduce(x, &j);
	z = r * r;
	/* cos(x) = sin(x + pi/2): two octants ahead of sinf */
	c = (j & 2) ? sin_poly(r, z) : cos_poly(z);
	return ((j + 2) & 4) ? -c : c;
}

static float apx_expf(float x)
{
	fbits scale;
	int n;
	float fn, r, p;

	/* n = round(x / ln 2) */
	n = (int)(x * LOG2EF + (x < 0.0f ? -0.5f : 0.5f));
	fn = (float)n;
	r = (x - fn * LN2HI) - fn * LN2LO;
	p = (((((1.9875691500e-4f * r + 1.3981999507e-3f) * r +
		8.3334519073e-3f) * r + 4.1665795894e-2f) * r +
	      1.6666665459e-1f) * r + 5.0000001201e-1f) * (r * r) + r + 1.0f;
	scale.u = (uint32_t)(n + 127) << 23; /* 2^n, |n| < 127 */
	return p * scale.f;
}

/* Uniform in [-scale, scale): 2 * u - 1 is exact, so one rounding. */
static float rand_sym(bench_rng *r, float scale)
{
	return scale * (2.0f * bench_rng_f01(r) - 1.0f);
}

static void mathapx_init(void *arena)
{
	float *xs = bench_at(arena, OFF_SIN);
	float *xc = bench_at(arena, OFF_COS);
	float *xe = bench_at(arena, OFF_EXP);
	float *es = bench_at(arena, OFF_ESCL);
	bench_rng r;
	int i;

	bench_rng_seed(&r, 0x6D617078u); /* "mapx" */
	for (i = 0; i < NEVAL; i++) {
		fbits s;

		xs[i] = rand_sym(&r, 100.0f);
		xc[i] = rand_sym(&r, 100.0f);
		xe[i] = rand_sym(&r, 80.0f);
		/* 2^-trunc(x * log2(e)): exp(x) * es[i] is in (0.5, 2) */
		s.u = (uint32_t)(127 - (int)(xe[i] * LOG2EF)) << 23;
		es[i] = s.f;
	}
}

static double mathapx_run(void *arena)
{
	const float *xs = bench_at(arena, OFF_SIN);
	const float *xc = bench_at(arena, OFF_COS);
	const float *xe = bench_at(arena, OFF_EXP);
	const float *es = bench_at(arena, OFF_ESCL);
	double ssin = 0.0, scos = 0.0, sexp = 0.0;
	int i;

	for (i = 0; i < NEVAL; i++)
		ssin += apx_sinf(xs[i]);
	for (i = 0; i < NEVAL; i++)
		scos += apx_cosf(xc[i]);
	for (i = 0; i < NEVAL; i++)
		sexp += apx_expf(xe[i]) * es[i];
	return ssin + scos + sexp;
}

BENCH_KDEF(mathapx) = {
	.name = "mathapx", .desc = "Cephes-style sinf/cosf/expf", .unit = "eval",
	.category = BENCH_CAT_FP, .flags = BENCH_F_APPROX, .tolerance = 1e-6,
	.units = 3 * NEVAL, .arena_bytes = ARENA_BYTES,
	.init = mathapx_init, .run = mathapx_run,
};
