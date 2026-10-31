/*
 * qmath.c - square roots and transcendental stand-ins for the qemu runner.
 *
 * sqrt/sqrtf: the G4 (-mcpu=7400) has no fsqrt, so GCC calls sqrt() and
 * sqrtf() for every square root.  These are exact integer digit-by-digit
 * square roots: the result is correctly rounded (round to nearest even), so
 * it is bit-identical to a hardware fsqrt/fsqrts and to the host's sqrt.
 * Special cases follow IEEE 754: sqrt(+-0) = +-0, sqrt(+inf) = +inf, NaN in
 * gives a quiet NaN out, negative arguments give the default NaN
 * (0x7ff8000000000000 / 0x7fc00000, what the PowerPC FPU produces).
 *
 * sin/cos/exp/log: simple and deterministic, for the LIB kernels' BLIB()
 * calls in the qemu build (both variants call the same code here, so the
 * accuracy only has to be reasonable): Cody-Waite argument reduction and
 * Taylor / atanh series with exactly folded rational coefficients.
 *
 * Compiled with -ffp-contract=off so the host unit test (-DQLG_HOST) runs
 * exactly the same arithmetic as the PPC build.
 */
#include <stdint.h>

#include "qmath.h"

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;

static inline u64 dbits(double d)
{
	union {
		double d;
		u64 u;
	} c;

	c.d = d;
	return c.u;
}

static inline double bitsd(u64 u)
{
	union {
		double d;
		u64 u;
	} c;

	c.u = u;
	return c.d;
}

static inline u32 fbits(float f)
{
	union {
		float f;
		u32 u;
	} c;

	c.f = f;
	return c.u;
}

static inline float bitsf(u32 u)
{
	union {
		float f;
		u32 u;
	} c;

	c.u = u;
	return c.f;
}

static inline int clz32(u32 x)
{
	return x ? __builtin_clz(x) : 32;
}

static inline int clz64(u64 x)
{
	u32 hi = (u32)(x >> 32);

	return hi ? clz32(hi) : 32 + clz32((u32)x);
}

/* ---- square roots ----------------------------------------------------------- */

double qm_sqrt(double x)
{
	u64 b = dbits(x), m, r, q, t;
	int eb = (int)((u32)(b >> 52) & 0x7ffu), e, half, i;

	if (eb == 0x7ff) {
		if (b << 12)                    /* NaN: quiet it */
			return bitsd(b | ((u64)1 << 51));
		if (b >> 63)                    /* -inf */
			return bitsd((u64)0x7ff8 << 48);
		return x;                       /* +inf */
	}
	if ((b << 1) == 0)                      /* +-0 */
		return x;
	if (b >> 63)
		return bitsd((u64)0x7ff8 << 48);

	m = b & (((u64)1 << 52) - 1);
	if (eb == 0) {                          /* subnormal: normalise */
		int s = clz64(m) - 11;

		m <<= s;
		e = -1022 - s;
	} else {
		m |= (u64)1 << 52;
		e = eb - 1023;
	}
	/* x = (m / 2^52) * 2^e.  Make the exponent even. */
	if (e & 1) {
		m <<= 1;
		e -= 1;
	}
	half = (e + 2048) / 2 - 1024;          /* e / 2, e even */

	/* R = floor(sqrt(m * 2^54)): 54 result bits = 53 + a rounding bit.
	 * Digit by digit, two radicand bits per step: 27 pairs from m (bits
	 * 53..0), then 27 pairs of zeros. */
	r = 0;
	q = 0;
	for (i = 52; i >= 0; i -= 2) {
		r = (r << 2) | ((m >> i) & 3u);
		t = (q << 2) | 1u;
		if (r >= t) {
			r -= t;
			q = (q << 1) | 1u;
		} else {
			q <<= 1;
		}
	}
	for (i = 0; i < 27; i++) {
		r <<= 2;
		t = (q << 2) | 1u;
		if (r >= t) {
			r -= t;
			q = (q << 1) | 1u;
		} else {
			q <<= 1;
		}
	}
	/* m * 2^54 is even and never the square of an odd number, so an exact
	 * half-way case cannot occur: round to nearest is "add the rounding
	 * bit".  A carry into bit 53 bumps the exponent through the addition. */
	q = (q + 1u) >> 1;
	return bitsd(((u64)(u32)(half + 1022) << 52) + q);
}

float qm_sqrtf(float x)
{
	u32 b = fbits(x), m, r, q, t;
	int eb = (int)((b >> 23) & 0xffu), e, half, i;

	if (eb == 0xff) {
		if (b << 9)
			return bitsf(b | 0x00400000u);
		if (b >> 31)
			return bitsf(0x7fc00000u);
		return x;
	}
	if ((b << 1) == 0)
		return x;
	if (b >> 31)
		return bitsf(0x7fc00000u);

	m = b & 0x007fffffu;
	if (eb == 0) {
		int s = clz32(m) - 8;

		m <<= s;
		e = -126 - s;
	} else {
		m |= 0x00800000u;
		e = eb - 127;
	}
	if (e & 1) {
		m <<= 1;
		e -= 1;
	}
	half = (e + 256) / 2 - 128;

	/* R = floor(sqrt(m * 2^25)) = floor(sqrt((2m) * 2^24)): 13 pairs from
	 * 2m (bits 25..0), then 12 pairs of zeros: 25 bits = 24 + rounding. */
	m <<= 1;
	r = 0;
	q = 0;
	for (i = 24; i >= 0; i -= 2) {
		r = (r << 2) | ((m >> i) & 3u);
		t = (q << 2) | 1u;
		if (r >= t) {
			r -= t;
			q = (q << 1) | 1u;
		} else {
			q <<= 1;
		}
	}
	for (i = 0; i < 12; i++) {
		r <<= 2;
		t = (q << 2) | 1u;
		if (r >= t) {
			r -= t;
			q = (q << 1) | 1u;
		} else {
			q <<= 1;
		}
	}
	q = (q + 1u) >> 1;
	return bitsf(((u32)(half + 126) << 23) + q);
}

#ifndef QLG_HOST
/* The names GCC calls (no fsqrt on -mcpu=7400). */
double sqrt(double x) __attribute__((alias("qm_sqrt")));
float sqrtf(float x) __attribute__((alias("qm_sqrtf")));
#endif

/* ---- sin / cos ---------------------------------------------------------------- */

/* fdlibm's split of pi/2 (33 + 33 + 33 bits + tail) and 2/pi:
 * pio2_1 + pio2_1t = pio2_1 + pio2_2 + pio2_3 + pio2_3t = pi/2. */
static const double invpio2 = 6.36619772367581382433e-01; /* 0x3FE45F30 6DC9C883 */
static const double pio2_1 = 1.57079632673412561417e+00;  /* 0x3FF921FB 54400000 */
static const double pio2_1t = 6.07710050650619224932e-11; /* 0x3DD0B461 1A626331 */
static const double pio2_2 = 6.07710050630396597660e-11;  /* 0x3DD0B461 1A600000 */
static const double pio2_3 = 2.02226624871116645580e-21;  /* 0x3BA3198A 2E000000 */
static const double pio2_3t = 8.47842766036889956997e-32; /* 0x397B839A 252049C1 */

/* sin(r), |r| <= pi/4 (a little more is fine): Taylor to r^17. */
static double ksin(double r)
{
	double z = r * r;
	double p = 1.0 / 355687428096000.0;

	p = p * z - 1.0 / 1307674368000.0;
	p = p * z + 1.0 / 6227020800.0;
	p = p * z - 1.0 / 39916800.0;
	p = p * z + 1.0 / 362880.0;
	p = p * z - 1.0 / 5040.0;
	p = p * z + 1.0 / 120.0;
	p = p * z - 1.0 / 6.0;
	return r + r * z * p;
}

/* cos(r), |r| <= pi/4: Taylor to r^18. */
static double kcos(double r)
{
	double z = r * r;
	double p = -1.0 / 6402373705728000.0;

	p = p * z + 1.0 / 20922789888000.0;
	p = p * z - 1.0 / 87178291200.0;
	p = p * z + 1.0 / 479001600.0;
	p = p * z - 1.0 / 3628800.0;
	p = p * z + 1.0 / 40320.0;
	p = p * z - 1.0 / 720.0;
	p = p * z + 1.0 / 24.0;
	return 1.0 - 0.5 * z + z * z * p;
}

/* x = n * pi/2 + r, |r| <= about pi/4; returns n mod 4. */
static int reduce(double x, double *r)
{
	double ax = x < 0 ? -x : x, fn;
	u32 quad;

	if (ax <= 0.78539816339744828) { /* pi/4 */
		*r = x;
		return 0;
	}
	if (ax < 1073741824.0) { /* 2^30: n fits an int, 3-part reduction */
		s32 n = (s32)(x * invpio2 + (x < 0 ? -0.5 : 0.5));

		fn = (double)n;
		if (n > -1048576 && n < 1048576)
			*r = (x - fn * pio2_1) - fn * pio2_1t;
		else
			*r = (((x - fn * pio2_1) - fn * pio2_2) - fn * pio2_3) - fn * pio2_3t;
		return (int)((u32)n & 3u);
	}
	/* Huge |x| (< 2^63 after the check below): n from a 2^52-rounded
	 * double, accuracy degrades with |x|.  Not used by the kernels. */
	fn = x * invpio2;
	if (fn > 4.0e15 || fn < -4.0e15) {
		*r = 0.0;
		return 0;
	}
	{
		double big = 6755399441055744.0; /* 1.5 * 2^52 */
		double t = fn + big;

		quad = (u32)dbits(t) & 3u;
		fn = t - big;
	}
	*r = (((x - fn * pio2_1) - fn * pio2_2) - fn * pio2_3) - fn * pio2_3t;
	return (int)quad;
}

double qm_sin(double x)
{
	double r;
	u32 ex = (u32)(dbits(x) >> 52) & 0x7ffu;

	if (ex == 0x7ffu)
		return x - x; /* NaN */
	if (ex < 1023u - 27u) /* |x| < 2^-27: sin x = x (keeps the sign of 0) */
		return x;
	switch (reduce(x, &r)) {
	case 0: return ksin(r);
	case 1: return kcos(r);
	case 2: return -ksin(r);
	default: return -kcos(r);
	}
}

double qm_cos(double x)
{
	double r;
	u32 ex = (u32)(dbits(x) >> 52) & 0x7ffu;

	if (ex == 0x7ffu)
		return x - x;
	switch (reduce(x, &r)) {
	case 0: return kcos(r);
	case 1: return -ksin(r);
	case 2: return -kcos(r);
	default: return ksin(r);
	}
}

/* ---- exp / log --------------------------------------------------------------- */

static const double ln2_hi = 6.93147180369123816490e-01; /* 0x3FE62E42 FEE00000 */
static const double ln2_lo = 1.90821492927058770002e-10; /* 0x3DEA39EF 35793C76 */
static const double invln2 = 1.44269504088896338700e+00; /* 0x3FF71547 652B82FE */

/* x * 2^k for |k| <= about 1100, by exact power-of-two factors. */
static double scale2(double x, int k)
{
	while (k > 1023) {
		x *= bitsd((u64)(1023 + 1023) << 52); /* 2^1023 */
		k -= 1023;
	}
	while (k < -1022) {
		x *= bitsd((u64)1 << 52); /* 2^-1022 */
		k += 1022;
	}
	return x * bitsd((u64)(u32)(k + 1023) << 52);
}

double qm_exp(double x)
{
	double r, p;
	s32 k;
	u32 ex = (u32)(dbits(x) >> 52) & 0x7ffu;

	if (ex == 0x7ffu) {
		if ((dbits(x) << 12) == 0 && (dbits(x) >> 63))
			return 0.0; /* exp(-inf) */
		return x + x;   /* +inf or NaN */
	}
	if (x > 709.782712893384)
		return bitsd((u64)0x7ff << 52);
	if (x < -745.1332191019412)
		return 0.0;

	k = (s32)(x * invln2 + (x < 0 ? -0.5 : 0.5));
	r = (x - (double)k * ln2_hi) - (double)k * ln2_lo;

	/* e^r, |r| <= ln2/2: Taylor to r^14 */
	p = 1.0 / 87178291200.0;
	p = p * r + 1.0 / 6227020800.0;
	p = p * r + 1.0 / 479001600.0;
	p = p * r + 1.0 / 39916800.0;
	p = p * r + 1.0 / 3628800.0;
	p = p * r + 1.0 / 362880.0;
	p = p * r + 1.0 / 40320.0;
	p = p * r + 1.0 / 5040.0;
	p = p * r + 1.0 / 720.0;
	p = p * r + 1.0 / 120.0;
	p = p * r + 1.0 / 24.0;
	p = p * r + 1.0 / 6.0;
	p = p * r + 0.5;
	p = 1.0 + (r + r * r * p);
	return scale2(p, k);
}

double qm_log(double x)
{
	u64 b = dbits(x);
	u32 ex = (u32)(b >> 52) & 0x7ffu;
	int k;
	double m, f, s, z, p;

	if (b >> 63) {
		if ((b << 1) == 0)
			return -bitsd((u64)0x7ff << 52); /* log(-0) = -inf */
		return bitsd((u64)0x7ff8 << 48);        /* NaN */
	}
	if (ex == 0x7ffu)
		return x + x; /* +inf, NaN */
	if (b == 0)
		return -bitsd((u64)0x7ff << 52);
	k = 0;
	if (ex == 0) { /* subnormal */
		x *= 18014398509481984.0; /* 2^54 */
		b = dbits(x);
		ex = (u32)(b >> 52) & 0x7ffu;
		k = -54;
	}
	k += (int)ex - 1023;
	m = bitsd((b & (((u64)1 << 52) - 1)) | ((u64)1023 << 52)); /* [1, 2) */
	if (m > 1.4142135623730951) { /* sqrt(2) */
		m *= 0.5;
		k++;
	}
	/* log(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.1716 */
	f = m - 1.0;
	s = f / (2.0 + f);
	z = s * s;
	p = 1.0 / 23.0;
	p = p * z + 1.0 / 21.0;
	p = p * z + 1.0 / 19.0;
	p = p * z + 1.0 / 17.0;
	p = p * z + 1.0 / 15.0;
	p = p * z + 1.0 / 13.0;
	p = p * z + 1.0 / 11.0;
	p = p * z + 1.0 / 9.0;
	p = p * z + 1.0 / 7.0;
	p = p * z + 1.0 / 5.0;
	p = p * z + 1.0 / 3.0;
	/* 2s(1 + z p) = f - s*(f - 2 z p s) ... keep it simple: */
	p = 2.0 * s + 2.0 * s * z * p;
	return (double)k * ln2_hi + ((double)k * ln2_lo + p);
}

float qm_sinf(float x)
{
	return (float)qm_sin((double)x);
}

float qm_cosf(float x)
{
	return (float)qm_cos((double)x);
}

float qm_expf(float x)
{
	return (float)qm_exp((double)x);
}

float qm_logf(float x)
{
	return (float)qm_log((double)x);
}
