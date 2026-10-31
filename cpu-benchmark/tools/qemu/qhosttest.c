/*
 * qhosttest.c - host unit test of the qemu runtime's C code.
 *
 * Builds qlibgcc.c, qmath.c and qblib.c with -DQLG_HOST (every helper is
 * renamed qlg_<name>, the math and libc stand-ins keep their qm_/qblib_
 * names) and compares them with native x86-64 arithmetic and the host
 * libc/libm on millions of random and edge-case inputs:
 *
 *   libgcc helpers   bit-exact against the native operators
 *   sqrt, sqrtf      bit-exact against the host's (correctly rounded) sqrt;
 *                    sqrtf exhaustively over every mantissa of an even and an
 *                    odd exponent and over all subnormals
 *   sin cos exp log  max error in ulps against glibc (must stay small)
 *   mem/str/qsort    results against glibc
 *
 *   qhosttest [-q]   exit status 0 = pass
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qmath.h"

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

/* qlibgcc.c under -DQLG_HOST */
u64 qlg___udivmoddi4(u64, u64, u64 *);
u64 qlg___udivdi3(u64, u64);
u64 qlg___umoddi3(u64, u64);
s64 qlg___divdi3(s64, s64);
s64 qlg___moddi3(s64, s64);
s64 qlg___divmoddi4(s64, s64, s64 *);
u64 qlg___ashldi3(u64, int);
u64 qlg___lshrdi3(u64, int);
s64 qlg___ashrdi3(s64, int);
u64 qlg___muldi3(u64, u64);
s64 qlg___negdi2(s64);
int qlg___cmpdi2(s64, s64);
int qlg___ucmpdi2(u64, u64);
int qlg___popcountsi2(u32);
int qlg___popcountdi2(u64);
int qlg___paritysi2(u32);
int qlg___paritydi2(u64);
int qlg___clzsi2(u32);
int qlg___clzdi2(u64);
int qlg___ctzsi2(u32);
int qlg___ctzdi2(u64);
int qlg___ffssi2(u32);
int qlg___ffsdi2(u64);
int qlg___clrsbsi2(s32);
int qlg___clrsbdi2(s64);
u32 qlg___bswapsi2(u32);
u64 qlg___bswapdi2(u64);
double qlg___floatdidf(s64);
double qlg___floatundidf(u64);
float qlg___floatdisf(s64);
float qlg___floatundisf(u64);
u64 qlg___fixunsdfdi(double);
s64 qlg___fixdfdi(double);
u64 qlg___fixunssfdi(float);
s64 qlg___fixsfdi(float);
u32 qlg___fixunsdfsi(double);
u32 qlg___fixunssfsi(float);
double qlg___powidf2(double, int);
float qlg___powisf2(float, int);

/* qblib.c under -DQLG_HOST */
void *qblib_memcpy(void *, const void *, size_t);
void *qblib_memmove(void *, const void *, size_t);
void *qblib_memset(void *, int, size_t);
int qblib_memcmp(const void *, const void *, size_t);
size_t qblib_strlen(const char *);
int qblib_strcmp(const char *, const char *);
char *qblib_strchr(const char *, int);
void qblib_qsort(void *, size_t, size_t, int (*)(const void *, const void *));

static int quiet;
static int failures;
static u64 rs = 0x243F6A8885A308D3ull;

static u64 rnd64(void)
{
	u64 z = (rs += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

static u64 rnd_wide(void)
{
	u64 v = rnd64();
	unsigned w = (unsigned)(rnd64() & 63u) + 1u;

	return w == 64 ? v : v >> (64u - w);
}

static u64 dbits(double d)
{
	u64 u;

	memcpy(&u, &d, 8);
	return u;
}

static double bitsd(u64 u)
{
	double d;

	memcpy(&d, &u, 8);
	return d;
}

static u32 fbits(float f)
{
	u32 u;

	memcpy(&u, &f, 4);
	return u;
}

static float bitsf(u32 u)
{
	float f;

	memcpy(&f, &u, 4);
	return f;
}

#define FAILF(...)                                                             \
	do {                                                                   \
		if (failures++ < 40)                                           \
			printf("  FAIL " __VA_ARGS__);                         \
	} while (0)

static void section(const char *name, int before, long cases)
{
	if (!quiet || failures != before)
		printf("%-34s %s (%ld cases)\n", name, failures == before ? "ok" : "FAILED", cases);
}

/* ---- integer helpers ------------------------------------------------------ */

static const u64 edge64[] = {
	0, 1, 2, 3, 7, 10, 0x7f, 0x80, 0xff, 0xffff, 0x7fffffff, 0x80000000ull,
	0xffffffffull, 0x100000000ull, 0x100000001ull, 0x1ffffffffull,
	0x7fffffffffffffffull, 0x8000000000000000ull, 0x8000000000000001ull,
	0xfffffffffffffffeull, 0xffffffffffffffffull, 0x0123456789abcdefull,
	0xfedcba9876543210ull, 1000000007ull, 0x00000000fffffffeull,
};
#define NEDGE (sizeof edge64 / sizeof edge64[0])

static void test_div(void)
{
	int before = failures;
	long n = 0;
	unsigned i, j;
	long k;

	for (i = 0; i < NEDGE; i++)
		for (j = 0; j < NEDGE; j++) {
			u64 a = edge64[i], b = edge64[j], r;
			s64 sa = (s64)a, sb = (s64)b, sr;

			if (!b)
				continue;
			n++;
			if (qlg___udivdi3(a, b) != a / b || qlg___umoddi3(a, b) != a % b ||
			    qlg___udivmoddi4(a, b, &r) != a / b || r != a % b)
				FAILF("udiv %llx / %llx\n", (unsigned long long)a, (unsigned long long)b);
			if (sa == INT64_MIN && sb == -1)
				continue;
			if (qlg___divdi3(sa, sb) != sa / sb || qlg___moddi3(sa, sb) != sa % sb ||
			    qlg___divmoddi4(sa, sb, &sr) != sa / sb || sr != sa % sb)
				FAILF("sdiv %lld / %lld\n", (long long)sa, (long long)sb);
		}
	for (k = 0; k < 4000000; k++) {
		u64 a = rnd_wide(), b = rnd_wide(), r;
		s64 sa, sb, sr;

		if (!b)
			b = 1;
		if (k & 1) /* quotients near powers of two and small remainders */
			a = b * (rnd64() & 0xffff) + (rnd64() % b);
		n++;
		if (qlg___udivdi3(a, b) != a / b || qlg___umoddi3(a, b) != a % b ||
		    qlg___udivmoddi4(a, b, &r) != a / b || r != a % b)
			FAILF("udiv %llx / %llx\n", (unsigned long long)a, (unsigned long long)b);
		sa = (rnd64() & 1) ? -(s64)(a >> 1) : (s64)a;
		sb = (rnd64() & 1) ? -(s64)(b >> 1) : (s64)b;
		if (!sb || (sa == INT64_MIN && sb == -1))
			continue;
		if (qlg___divdi3(sa, sb) != sa / sb || qlg___moddi3(sa, sb) != sa % sb ||
		    qlg___divmoddi4(sa, sb, &sr) != sa / sb || sr != sa % sb)
			FAILF("sdiv %lld / %lld\n", (long long)sa, (long long)sb);
	}
	section("64-bit divide / modulo", before, n);
}

static int cmp3(s64 a, s64 b)
{
	return a < b ? 0 : a > b ? 2 : 1;
}

static int ucmp3(u64 a, u64 b)
{
	return a < b ? 0 : a > b ? 2 : 1;
}

static void test_arith(void)
{
	int before = failures;
	long n = 0, k;
	int s;

	for (k = 0; k < 200000; k++) {
		u64 a = k < (long)NEDGE ? edge64[k] : rnd_wide(), b = rnd_wide();

		for (s = 0; s < 64; s++) {
			n++;
			if (qlg___ashldi3(a, s) != a << s || qlg___lshrdi3(a, s) != a >> s ||
			    qlg___ashrdi3((s64)a, s) != (s64)a >> s)
				FAILF("shift %llx by %d\n", (unsigned long long)a, s);
		}
		if (qlg___muldi3(a, b) != a * b)
			FAILF("mul %llx * %llx\n", (unsigned long long)a, (unsigned long long)b);
		if (qlg___negdi2((s64)a) != (s64)(0 - a))
			FAILF("neg %llx\n", (unsigned long long)a);
		if (qlg___cmpdi2((s64)a, (s64)b) != cmp3((s64)a, (s64)b) ||
		    qlg___ucmpdi2(a, b) != ucmp3(a, b) || qlg___cmpdi2((s64)a, (s64)a) != 1 ||
		    qlg___ucmpdi2(a, a ^ 1) != ucmp3(a, a ^ 1))
			FAILF("cmp %llx %llx\n", (unsigned long long)a, (unsigned long long)b);
	}
	section("64-bit shifts / mul / neg / cmp", before, n);
}

static void test_bits(void)
{
	int before = failures;
	long n = 0, k;

	for (k = 0; k < 3000000; k++) {
		u64 a = k < (long)NEDGE ? edge64[k] : rnd_wide();
		u32 x = (u32)(k < (long)NEDGE ? edge64[k] : rnd_wide());

		if (k & 1)
			a = ~a;
		n++;
		if (qlg___popcountsi2(x) != __builtin_popcount(x) ||
		    qlg___paritysi2(x) != __builtin_parity(x) ||
		    qlg___clrsbsi2((s32)x) != __builtin_clrsb((s32)x) ||
		    qlg___ffssi2(x) != __builtin_ffs((int)x) ||
		    qlg___bswapsi2(x) != __builtin_bswap32(x) ||
		    (x && (qlg___clzsi2(x) != __builtin_clz(x) || qlg___ctzsi2(x) != __builtin_ctz(x))))
			FAILF("bits32 %08x\n", x);
		if (qlg___popcountdi2(a) != __builtin_popcountll(a) ||
		    qlg___paritydi2(a) != __builtin_parityll(a) ||
		    qlg___clrsbdi2((s64)a) != __builtin_clrsbll((s64)a) ||
		    qlg___ffsdi2(a) != __builtin_ffsll((long long)a) ||
		    qlg___bswapdi2(a) != __builtin_bswap64(a) ||
		    (a && (qlg___clzdi2(a) != __builtin_clzll(a) || qlg___ctzdi2(a) != __builtin_ctzll(a))))
			FAILF("bits64 %016llx\n", (unsigned long long)a);
	}
	section("popcount/parity/clz/ctz/ffs/clrsb/bswap", before, n);
}

/* ---- conversions --------------------------------------------------------- */

static void check_i2f(u64 u, long *n)
{
	s64 s = (s64)u;

	(*n)++;
	if (dbits(qlg___floatdidf(s)) != dbits((double)s) ||
	    dbits(qlg___floatundidf(u)) != dbits((double)u) ||
	    fbits(qlg___floatdisf(s)) != fbits((float)s) ||
	    fbits(qlg___floatundisf(u)) != fbits((float)u))
		FAILF("int->fp %016llx: %a %a %a %a\n", (unsigned long long)u,
		      qlg___floatdidf(s), qlg___floatundidf(u), (double)qlg___floatdisf(s),
		      (double)qlg___floatundisf(u));
}

static void test_int2fp(void)
{
	int before = failures;
	long n = 0, k;
	int e, d;

	for (k = 0; k < (long)NEDGE; k++) {
		check_i2f(edge64[k], &n);
		check_i2f(0 - edge64[k], &n);
	}
	/* half-way and sticky patterns around the 24- and 53-bit rounding points */
	for (e = 0; e < 64; e++)
		for (d = -3; d <= 3; d++) {
			u64 base = (u64)1 << e;

			check_i2f(base + (u64)(s64)d, &n);
			check_i2f(0 - (base + (u64)(s64)d), &n);
			if (e >= 25) {
				check_i2f(base | ((u64)1 << (e - 24)) | (u64)(s64)d, &n);
				check_i2f((base | ((u64)1 << (e - 25))) + (u64)(s64)d, &n);
			}
			if (e >= 54) {
				check_i2f((base | ((u64)1 << (e - 53))) + (u64)(s64)d, &n);
				check_i2f((base | ((u64)1 << (e - 54))) + (u64)(s64)d, &n);
				check_i2f(0 - ((base | ((u64)1 << (e - 54))) + (u64)(s64)d), &n);
			}
		}
	for (k = 0; k < 4000000; k++)
		check_i2f(rnd_wide(), &n);
	section("int64 -> double/float", before, n);
}

static void check_f2i(double d, long *n)
{
	(*n)++;
	if (d > -1.0 && d < 18446744073709551616.0 && d >= 0.0) {
		if (qlg___fixunsdfdi(d) != (u64)d)
			FAILF("fixunsdfdi %a\n", d);
		if (d < 4294967296.0 && qlg___fixunsdfsi(d) != (u32)d)
			FAILF("fixunsdfsi %a\n", d);
	}
	if (d > -9223372036854775809.0 && d < 9223372036854775808.0) {
		if (qlg___fixdfdi(d) != (s64)d)
			FAILF("fixdfdi %a\n", d);
	}
	{
		float f = (float)d;

		if (f >= 0.0f && f < 18446744073709551616.0f) {
			if (qlg___fixunssfdi(f) != (u64)f)
				FAILF("fixunssfdi %a\n", (double)f);
			if (f < 4294967296.0f && qlg___fixunssfsi(f) != (u32)f)
				FAILF("fixunssfsi %a\n", (double)f);
		}
		if (f >= -9223372036854775808.0f && f < 9223372036854775808.0f) {
			if (qlg___fixsfdi(f) != (s64)f)
				FAILF("fixsfdi %a\n", (double)f);
		}
	}
}

static void test_fp2int(void)
{
	int before = failures;
	long n = 0, k;
	int e;

	for (e = -60; e < 65; e++) {
		double p = ldexp(1.0, e);

		check_f2i(p, &n);
		check_f2i(-p, &n);
		check_f2i(nextafter(p, 0.0), &n);
		check_f2i(-nextafter(p, 0.0), &n);
		check_f2i(nextafter(p, INFINITY), &n);
		check_f2i(p + 0.5, &n);
		check_f2i(p - 0.5, &n);
	}
	check_f2i(0.0, &n);
	check_f2i(-0.0, &n);
	for (k = 0; k < 4000000; k++) {
		/* random magnitude in [2^-8, 2^64) */
		u64 b = ((u64)(1023 - 8 + rnd64() % 72) << 52) | (rnd64() & 0xfffffffffffffull);
		double d = bitsd(b);

		check_f2i(d, &n);
		check_f2i(-d, &n);
	}
	section("double/float -> int64/uint64/uint32", before, n);
}

static void test_powi(void)
{
	int before = failures;
	long n = 0, k;

	for (k = 0; k < 2000000; k++) {
		double x = (k & 1) ? 0.5 + (double)(rnd64() >> 12) * 0x1p-52 * 1.5
				   : bitsd(rnd64() & 0xbfffffffffffffffull);
		int m = (int)(rnd64() % 129u) - 64;
		double r1 = qlg___powidf2(x, m), r2 = __builtin_powi(x, m);
		float xf = (float)x;
		float f1 = qlg___powisf2(xf, m), f2 = __builtin_powif(xf, m);

		n++;
		if (dbits(r1) != dbits(r2) && !(r1 != r1 && r2 != r2))
			FAILF("powidf2 %a %d: %a vs %a\n", x, m, r1, r2);
		if (fbits(f1) != fbits(f2) && !(f1 != f1 && f2 != f2))
			FAILF("powisf2 %a %d: %a vs %a\n", (double)xf, m, (double)f1, (double)f2);
	}
	section("powi (double, float)", before, n);
}

/* ---- square roots --------------------------------------------------------- */

static int same_d(double a, double b)
{
	return dbits(a) == dbits(b) || (a != a && b != b);
}

static int same_f(float a, float b)
{
	return fbits(a) == fbits(b) || (a != a && b != b);
}

static void test_sqrt(void)
{
	int before = failures;
	long n = 0, k;
	u32 m;
	static const u64 sp[] = {
		0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
		0x7ff8000000000000ull, 0x7ff0000000000001ull, 0xfff8000000000000ull,
		1, 2, 3, 0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
		0x3ff0000000000000ull, 0xbff0000000000000ull, 0x8000000000000001ull,
	};

	for (k = 0; k < (long)(sizeof sp / sizeof sp[0]); k++) {
		double x = bitsd(sp[k]);

		n++;
		if (!same_d(qm_sqrt(x), sqrt(x)))
			FAILF("sqrt %016llx: %a vs %a\n", (unsigned long long)sp[k], qm_sqrt(x), sqrt(x));
		if ((qm_sqrt(x) != qm_sqrt(x)) && (dbits(qm_sqrt(x)) & 0x0008000000000000ull) == 0)
			FAILF("sqrt %016llx: NaN not quiet\n", (unsigned long long)sp[k]);
	}
	for (k = 0; k < 20000000; k++) {
		u64 b = rnd64();
		double x;

		if (k & 1)
			b >>= rnd64() % 64u;
		if (k % 3 == 0) { /* squares of 26/27-bit integers, +-1 ulp: exact and near-exact */
			double r = (double)(rnd64() >> 37);

			b = dbits(r * r) + (rnd64() % 3u) - 1u;
		}
		x = bitsd(b & 0x7fffffffffffffffull);
		n++;
		if (!same_d(qm_sqrt(x), sqrt(x)))
			FAILF("sqrt %016llx: %a vs %a\n", (unsigned long long)b, qm_sqrt(x), sqrt(x));
	}
	section("sqrt (bit-exact vs host)", before, n);

	before = failures;
	n = 0;
	/* every mantissa of exponent 127 (even) and 128 (odd), all subnormals,
	 * the top exponent, plus random patterns and all specials */
	for (m = 0; m < 0x800000u; m++) {
		static const u32 ex[] = { 127u << 23, 128u << 23, 0, 254u << 23, 1u << 23 };
		unsigned e;

		for (e = 0; e < sizeof ex / sizeof ex[0]; e++) {
			float x = bitsf(ex[e] | m);

			n++;
			if (!same_f(qm_sqrtf(x), sqrtf(x)))
				FAILF("sqrtf %08x: %a vs %a\n", ex[e] | m, (double)qm_sqrtf(x), (double)sqrtf(x));
		}
	}
	for (k = 0; k < 4000000; k++) {
		u32 b = (u32)rnd64();
		float x = bitsf(b);

		n++;
		if (!same_f(qm_sqrtf(x), sqrtf(x)))
			FAILF("sqrtf %08x: %a vs %a\n", b, (double)qm_sqrtf(x), (double)sqrtf(x));
	}
	section("sqrtf (bit-exact vs host)", before, n);
}

/* ---- transcendental stand-ins ---------------------------------------------- */

static double ulp_err(double got, double ref)
{
	int e;

	if (got == ref || (got != got && ref != ref))
		return 0.0;
	if (got != got || ref != ref || isinf(got) || isinf(ref))
		return 1e30;
	if (ref == 0.0)
		return fabs(got) / 4.9406564584124654e-324;
	frexp(ref, &e);
	if (e - 53 < -1074)
		e = -1074 + 53;
	return fabs(got - ref) / ldexp(1.0, e - 53);
}

static double ulp_errf(float got, float ref)
{
	int e;

	if (got == ref || (got != got && ref != ref))
		return 0.0;
	if (got != got || ref != ref || isinf(got) || isinf(ref))
		return 1e30;
	frexp(ref, &e);
	if (e - 24 < -149)
		e = -149 + 24;
	return fabs((double)got - (double)ref) / ldexp(1.0, e - 24);
}

struct mtest {
	const char *name;
	double (*mine)(double);
	double (*ref)(double);
	float (*minef)(float);
	float (*reff)(float);
	double lo, hi;
	double max_ulp, max_ulpf;
};

static void test_math(void)
{
	static const struct mtest t[] = {
		{ "sin [-pi,pi]", qm_sin, sin, qm_sinf, sinf, -3.15, 3.15, 4, 1 },
		{ "sin [-1e3,1e3]", qm_sin, sin, qm_sinf, sinf, -1e3, 1e3, 4, 1 },
		{ "sin [-1e6,1e6]", qm_sin, sin, qm_sinf, sinf, -1e6, 1e6, 64, 1 },
		{ "cos [-pi,pi]", qm_cos, cos, qm_cosf, cosf, -3.15, 3.15, 4, 1 },
		{ "cos [-1e3,1e3]", qm_cos, cos, qm_cosf, cosf, -1e3, 1e3, 4, 1 },
		{ "exp [-745,710]", qm_exp, exp, qm_expf, expf, -745.0, 709.7, 4, 1 },
		{ "exp [-1,1]", qm_exp, exp, qm_expf, expf, -1.0, 1.0, 4, 1 },
		{ "log (0,1e300]", qm_log, log, qm_logf, logf, 1e-300, 1e300, 4, 1 },
		{ "log [0.5,2]", qm_log, log, qm_logf, logf, 0.5, 2.0, 4, 1 },
	};
	unsigned i;
	long k;

	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		int before = failures;
		double worst = 0, worstf = 0, wx = 0, wxf = 0;

		for (k = 0; k < 400000; k++) {
			double x, e, ef;
			float xf;

			if (t[i].lo > 0) /* log-uniform for log */
				x = exp(log(t[i].lo) + (log(t[i].hi) - log(t[i].lo)) * (double)(rnd64() >> 11) * 0x1p-53);
			else
				x = t[i].lo + (t[i].hi - t[i].lo) * (double)(rnd64() >> 11) * 0x1p-53;
			xf = (float)x;
			e = ulp_err(t[i].mine(x), t[i].ref(x));
			ef = ulp_errf(t[i].minef(xf), t[i].reff(xf));
			if (e > worst) {
				worst = e;
				wx = x;
			}
			if (ef > worstf) {
				worstf = ef;
				wxf = xf;
			}
		}
		if (worst > t[i].max_ulp)
			FAILF("%s: %.2f ulp at %a\n", t[i].name, worst, wx);
		if (worstf > t[i].max_ulpf)
			FAILF("%sf: %.2f ulp at %a\n", t[i].name, worstf, wxf);
		if (!quiet || failures != before)
			printf("%-34s %s (max %.2f ulp double, %.2f ulp float)\n", t[i].name,
			       failures == before ? "ok" : "FAILED", worst, worstf);
	}
	{
		/* IEEE special cases must match exactly (NaN: any NaN). */
		int before = failures;
		const double inf = INFINITY;
		const struct {
			const char *what;
			double got, want;
		} sp[] = {
			{ "exp(0)", qm_exp(0.0), 1.0 },       { "exp(-0)", qm_exp(-0.0), 1.0 },
			{ "exp(inf)", qm_exp(inf), inf },     { "exp(-inf)", qm_exp(-inf), 0.0 },
			{ "exp(nan)", qm_exp(NAN), NAN },     { "exp(800)", qm_exp(800.0), inf },
			{ "exp(-800)", qm_exp(-800.0), 0.0 }, { "log(1)", qm_log(1.0), 0.0 },
			{ "log(0)", qm_log(0.0), -inf },      { "log(-0)", qm_log(-0.0), -inf },
			{ "log(inf)", qm_log(inf), inf },     { "log(-1)", qm_log(-1.0), NAN },
			{ "log(nan)", qm_log(NAN), NAN },     { "sin(0)", qm_sin(0.0), 0.0 },
			{ "sin(-0)", qm_sin(-0.0), -0.0 },    { "sin(inf)", qm_sin(inf), NAN },
			{ "sin(nan)", qm_sin(NAN), NAN },     { "cos(0)", qm_cos(0.0), 1.0 },
			{ "cos(inf)", qm_cos(-inf), NAN },    { "cos(nan)", qm_cos(NAN), NAN },
			{ "log(denorm)", qm_log(1e-310), log(1e-310) },
		};
		unsigned j;

		for (j = 0; j < sizeof sp / sizeof sp[0]; j++)
			if (!same_d(sp[j].got, sp[j].want))
				FAILF("%s = %a, want %a\n", sp[j].what, sp[j].got, sp[j].want);
		section("math specials", before, (long)(sizeof sp / sizeof sp[0]));
	}
}

/* ---- libc stand-ins ------------------------------------------------------- */

static int sgn(int x)
{
	return (x > 0) - (x < 0);
}

static int cmp_u32(const void *a, const void *b)
{
	u32 x = *(const u32 *)a, y = *(const u32 *)b;

	return (x > y) - (x < y);
}

static int cmp_rec(const void *a, const void *b) /* 7-byte records, key = first 2 bytes */
{
	const unsigned char *x = a, *y = b;
	int kx = x[0] << 8 | x[1], ky = y[0] << 8 | y[1];

	return (kx > ky) - (kx < ky);
}

static void test_libc(void)
{
	static unsigned char a[4096], b[4096], c[4096];
	int before = failures;
	long n = 0, k;

	for (k = 0; k < 200000; k++) {
		size_t len = rnd64() % 600, off1 = rnd64() % 64, off2 = rnd64() % 64;
		int v = (int)(rnd64() & 0x1ff) - 0x80;
		size_t i;

		for (i = 0; i < sizeof a; i++)
			a[i] = b[i] = c[i] = (unsigned char)rnd64();
		n++;
		/* memcpy */
		memcpy(b, a, sizeof a);
		qblib_memcpy(b + off1, a + off2, len);
		memcpy(c + off1, a + off2, len);
		memcpy(c, b, off1);
		memcpy(c + off1 + len, b + off1 + len, sizeof a - off1 - len);
		if (memcmp(b, c, sizeof a) || qblib_memcpy(b, a, 0) != b)
			FAILF("memcpy len %zu off %zu/%zu\n", len, off1, off2);
		/* memmove (overlapping both ways) */
		memcpy(b, a, sizeof a);
		memcpy(c, a, sizeof a);
		if (qblib_memmove(b + off1, b + off2, len) != b + off1)
			FAILF("memmove return\n");
		memmove(c + off1, c + off2, len);
		if (memcmp(b, c, sizeof a))
			FAILF("memmove len %zu off %zu/%zu\n", len, off1, off2);
		/* memset */
		memcpy(b, a, sizeof a);
		memcpy(c, a, sizeof a);
		qblib_memset(b + off1, v, len);
		memset(c + off1, v, len);
		if (memcmp(b, c, sizeof a))
			FAILF("memset len %zu off %zu v %d\n", len, off1, v);
		/* memcmp */
		memcpy(b, a, sizeof a);
		if (len && (rnd64() & 1))
			b[off2 + rnd64() % len] ^= (unsigned char)(1u << (rnd64() & 7));
		if (sgn(qblib_memcmp(a + off2, b + off2, len)) != sgn(memcmp(a + off2, b + off2, len)))
			FAILF("memcmp len %zu\n", len);
		/* strings */
		for (i = 0; i < 80; i++) {
			a[i] = (unsigned char)(1 + rnd64() % ((k & 1) ? 3 : 255));
			b[i] = (rnd64() % 4) ? a[i] : (unsigned char)(1 + rnd64() % 255);
		}
		a[rnd64() % 80] = 0;
		b[rnd64() % 80] = 0;
		a[80] = b[80] = 0;
		if (qblib_strlen((char *)a) != strlen((char *)a) ||
		    sgn(qblib_strcmp((char *)a, (char *)b)) != sgn(strcmp((char *)a, (char *)b)) ||
		    qblib_strchr((char *)a, v) != strchr((char *)a, v) ||
		    qblib_strchr((char *)a, 0) != strchr((char *)a, 0) ||
		    qblib_strchr((char *)a, a[3]) != strchr((char *)a, a[3]))
			FAILF("string ops\n");
	}
	section("memcpy/memmove/memset/memcmp/str*", before, n);

	before = failures;
	n = 0;
	for (k = 0; k < 3000; k++) {
		static u32 x[5000], y[5000];
		static unsigned char r1[7 * 700], r2[7 * 700];
		size_t m = (size_t)(rnd64() % 5000), i;
		u32 range = (k & 1) ? 16u : 0xffffffffu;

		for (i = 0; i < m; i++)
			x[i] = y[i] = (u32)rnd64() % range;
		qblib_qsort(x, m, 4, cmp_u32);
		qsort(y, m, 4, cmp_u32);
		n++;
		if (memcmp(x, y, m * 4))
			FAILF("qsort u32 n %zu\n", m);
		/* odd element size: sorted by key, and a permutation of the input */
		m = (size_t)(rnd64() % 700);
		for (i = 0; i < m * 7; i++)
			r1[i] = (unsigned char)rnd64();
		for (i = 0; i < m; i++)
			r1[i * 7 + 6] = (unsigned char)i, r1[i * 7 + 5] = (unsigned char)(i >> 8);
		memcpy(r2, r1, m * 7);
		qblib_qsort(r1, m, 7, cmp_rec);
		{
			static unsigned char seen[700];
			int bad = 0;

			memset(seen, 0, sizeof seen);
			for (i = 0; i < m; i++) {
				size_t id = (size_t)r1[i * 7 + 6] | (size_t)r1[i * 7 + 5] << 8;

				if (id >= m || seen[id]++ || memcmp(r1 + i * 7, r2 + id * 7, 7))
					bad = 1;
				if (i && cmp_rec(r1 + (i - 1) * 7, r1 + i * 7) > 0)
					bad = 1;
			}
			if (bad)
				FAILF("qsort 7-byte records n %zu\n", m);
		}
	}
	section("qsort", before, n);
}

int main(int argc, char **argv)
{
	quiet = argc > 1 && !strcmp(argv[1], "-q");
	test_div();
	test_arith();
	test_bits();
	test_int2fp();
	test_fp2int();
	test_powi();
	test_sqrt();
	test_math();
	test_libc();
	printf("qhosttest: %s (%d failure%s)\n", failures ? "FAILED" : "PASS", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
