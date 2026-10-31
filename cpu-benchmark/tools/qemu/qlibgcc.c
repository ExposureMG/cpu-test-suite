/*
 * qlibgcc.c - minimal C versions of the libgcc helpers that 32-bit-only code
 * (-m32 -mcpu=7400 -mno-powerpc64) needs.  The qemu runner never links a
 * toolchain libgcc.a: testxenon3's 32-bit multilib is built for the Xenon and
 * may contain 64-bit instructions that qemu-ppc (a G4) cannot execute.
 *
 * Which helpers are needed was established with "xenon-nm -u" over test code
 * at -O0/-O1/-O2/-O3/-Os for both toolchains; this file covers the union:
 *   64-bit divide / modulo, shifts, multiply, negate, compare,
 *   popcount / parity / clz / ctz / ffs / clrsb / bswap (si2, di2),
 *   int64 <-> double / float conversions, double / float -> uint32,
 *   powi (double, float).
 *
 * Every helper is written with 32-bit word operations only, so it can never
 * call itself, and the build checks that this object has no undefined
 * symbols.  -DQLG_HOST renames everything to qlg_<name> so qhosttest.c can
 * compare the same code against native host arithmetic.
 *
 * Results follow libgcc's semantics (round to nearest for int->FP, truncation
 * for FP->int; powi uses libgcc's exact square-and-multiply order, so results
 * are bit-identical to libgcc's).  Undefined inputs (division by zero,
 * out-of-range or NaN conversions) return a fixed value instead of trapping.
 */
#include <stdint.h>

#ifdef QLG_HOST
#define QLG(name) qlg_##name
#else
#define QLG(name) name
#endif

typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

#define HI(x) ((u32)((u64)(x) >> 32))
#define LO(x) ((u32)(u64)(x))

static inline u64 mk64(u32 hi, u32 lo)
{
	return ((u64)hi << 32) | lo;
}

static inline int clz32(u32 x)
{
	return x ? __builtin_clz(x) : 32; /* cntlzw */
}

static inline int ctz32(u32 x)
{
	return x ? 31 - __builtin_clz(x & (0u - x)) : 32;
}

static inline int clz64(u64 x)
{
	return HI(x) ? clz32(HI(x)) : 32 + clz32(LO(x));
}

static inline int ctz64(u64 x)
{
	return LO(x) ? ctz32(LO(x)) : 32 + ctz32(HI(x));
}

/* 64-bit compares from 32-bit words. */
static inline int ult64(u64 a, u64 b)
{
	return HI(a) < HI(b) || (HI(a) == HI(b) && LO(a) < LO(b));
}

static inline u64 sub64(u64 a, u64 b)
{
	u32 lo = LO(a) - LO(b);
	u32 hi = HI(a) - HI(b) - (LO(a) < LO(b));

	return mk64(hi, lo);
}

static inline u64 shl64(u64 a, int b)
{
	u32 hi = HI(a), lo = LO(a);

	b &= 63;
	if (b == 0)
		return a;
	if (b >= 32)
		return mk64(lo << (b - 32), 0);
	return mk64((hi << b) | (lo >> (32 - b)), lo << b);
}

static inline u64 shr64(u64 a, int b)
{
	u32 hi = HI(a), lo = LO(a);

	b &= 63;
	if (b == 0)
		return a;
	if (b >= 32)
		return mk64(0, hi >> (b - 32));
	return mk64(hi >> b, (lo >> b) | (hi << (32 - b)));
}

static inline u64 sar64(u64 a, int b)
{
	s32 hi = (s32)HI(a);
	u32 lo = LO(a);

	b &= 63;
	if (b == 0)
		return a;
	if (b >= 32)
		return mk64((u32)(hi >> 31), (u32)(hi >> (b - 32)));
	return mk64((u32)(hi >> b), (lo >> b) | ((u32)hi << (32 - b)));
}

static inline u64 neg64(u64 a)
{
	return sub64(0, a);
}

/* ---- division --------------------------------------------------------------- */

/* Restoring shift-subtract division, quotient bits from the top. */
static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
	u64 q = 0;
	int sh;

	if (d == 0) { /* undefined: no trap, fixed result */
		if (rem)
			*rem = n;
		return ~(u64)0;
	}
	if (HI(n) == 0 && HI(d) == 0) {
		u32 nl = LO(n), dl = LO(d), ql = nl / dl; /* divwu */

		if (rem)
			*rem = nl - ql * dl;
		return ql;
	}
	if (ult64(n, d)) {
		if (rem)
			*rem = n;
		return 0;
	}
	sh = clz64(d) - clz64(n); /* 0..63: d << sh has the same top bit as n */
	d = shl64(d, sh);
	for (;;) {
		if (!ult64(n, d)) {
			n = sub64(n, d);
			q |= 1;
		}
		if (sh-- == 0)
			break;
		q = shl64(q, 1);
		d = shr64(d, 1);
	}
	if (rem)
		*rem = n;
	return q;
}

u64 QLG(__udivmoddi4)(u64 n, u64 d, u64 *rem)
{
	return udivmod64(n, d, rem);
}

u64 QLG(__udivdi3)(u64 n, u64 d)
{
	return udivmod64(n, d, 0);
}

u64 QLG(__umoddi3)(u64 n, u64 d)
{
	u64 r;

	udivmod64(n, d, &r);
	return r;
}

static inline u64 abs64(s64 a)
{
	return (HI(a) >> 31) ? neg64((u64)a) : (u64)a;
}

s64 QLG(__divdi3)(s64 a, s64 b)
{
	u64 q = udivmod64(abs64(a), abs64(b), 0);

	if ((HI(a) ^ HI(b)) >> 31)
		q = neg64(q);
	return (s64)q;
}

s64 QLG(__moddi3)(s64 a, s64 b)
{
	u64 r;

	udivmod64(abs64(a), abs64(b), &r);
	if (HI(a) >> 31)
		r = neg64(r);
	return (s64)r;
}

s64 QLG(__divmoddi4)(s64 a, s64 b, s64 *rem)
{
	u64 r, q = udivmod64(abs64(a), abs64(b), &r);

	if ((HI(a) ^ HI(b)) >> 31)
		q = neg64(q);
	if (HI(a) >> 31)
		r = neg64(r);
	if (rem)
		*rem = (s64)r;
	return (s64)q;
}

/* ---- shifts, multiply, negate, compare (-Os and -O0 code) ------------------ */

u64 QLG(__ashldi3)(u64 a, int b)
{
	return shl64(a, b);
}

u64 QLG(__lshrdi3)(u64 a, int b)
{
	return shr64(a, b);
}

s64 QLG(__ashrdi3)(s64 a, int b)
{
	return (s64)sar64((u64)a, b);
}

/* 32x32 -> 64 from 16-bit halves (no reliance on mulhwu patterns). */
static inline u64 mul32x32(u32 a, u32 b)
{
	u32 a0 = a & 0xffffu, a1 = a >> 16, b0 = b & 0xffffu, b1 = b >> 16;
	u32 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	u32 mid = (p00 >> 16) + (p01 & 0xffffu) + (p10 & 0xffffu);
	u32 lo = (p00 & 0xffffu) | (mid << 16);
	u32 hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);

	return mk64(hi, lo);
}

u64 QLG(__muldi3)(u64 a, u64 b)
{
	u64 p = mul32x32(LO(a), LO(b));

	return mk64(HI(p) + LO(a) * HI(b) + HI(a) * LO(b), LO(p));
}

s64 QLG(__negdi2)(s64 a)
{
	return (s64)neg64((u64)a);
}

int QLG(__cmpdi2)(s64 a, s64 b)
{
	s32 ah = (s32)HI(a), bh = (s32)HI(b);

	if (ah < bh)
		return 0;
	if (ah > bh)
		return 2;
	if (LO(a) < LO(b))
		return 0;
	if (LO(a) > LO(b))
		return 2;
	return 1;
}

int QLG(__ucmpdi2)(u64 a, u64 b)
{
	if (HI(a) < HI(b))
		return 0;
	if (HI(a) > HI(b))
		return 2;
	if (LO(a) < LO(b))
		return 0;
	if (LO(a) > LO(b))
		return 2;
	return 1;
}

/* ---- bit counting --------------------------------------------------------- */

static inline int popcount32(u32 x)
{
	x = x - ((x >> 1) & 0x55555555u);
	x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
	x = (x + (x >> 4)) & 0x0f0f0f0fu;
	x = x + (x >> 8);
	x = x + (x >> 16);
	return (int)(x & 0x3fu);
}

static inline int parity32(u32 x)
{
	x ^= x >> 16;
	x ^= x >> 8;
	x ^= x >> 4;
	return (int)((0x6996u >> (x & 15u)) & 1u);
}

int QLG(__popcountsi2)(u32 x)
{
	return popcount32(x);
}

int QLG(__popcountdi2)(u64 x)
{
	return popcount32(HI(x)) + popcount32(LO(x));
}

int QLG(__paritysi2)(u32 x)
{
	return parity32(x);
}

int QLG(__paritydi2)(u64 x)
{
	return parity32(HI(x) ^ LO(x));
}

/* clz/ctz of 0 are undefined in libgcc; these return the bit width. */
int QLG(__clzsi2)(u32 x)
{
	return clz32(x);
}

int QLG(__clzdi2)(u64 x)
{
	return clz64(x);
}

int QLG(__ctzsi2)(u32 x)
{
	return ctz32(x);
}

int QLG(__ctzdi2)(u64 x)
{
	return ctz64(x);
}

int QLG(__ffssi2)(u32 x)
{
	return x ? ctz32(x) + 1 : 0;
}

int QLG(__ffsdi2)(u64 x)
{
	return (HI(x) | LO(x)) ? ctz64(x) + 1 : 0;
}

int QLG(__clrsbsi2)(s32 x)
{
	u32 y = (u32)x;

	if (y >> 31)
		y = ~y;
	return y ? clz32(y) - 1 : 31;
}

int QLG(__clrsbdi2)(s64 x)
{
	u32 hi = HI(x), lo = LO(x);

	if (hi >> 31) {
		hi = ~hi;
		lo = ~lo;
	}
	if (hi)
		return clz32(hi) - 1;
	if (lo)
		return 31 + clz32(lo);
	return 63;
}

static inline u32 bswap32(u32 x)
{
	return (x >> 24) | ((x >> 8) & 0xff00u) | ((x & 0xff00u) << 8) | (x << 24);
}

u32 QLG(__bswapsi2)(u32 x)
{
	return bswap32(x);
}

u64 QLG(__bswapdi2)(u64 x)
{
	return mk64(bswap32(LO(x)), bswap32(HI(x)));
}

/* ---- integer -> floating point (round to nearest) ------------------------- */

#define TWO32 4294967296.0

/* hi * 2^32 is exact and lo is exact, so the one addition (or fused
 * multiply-add) rounds the exact value once: correctly rounded. */
double QLG(__floatdidf)(s64 a)
{
	return (double)(s32)HI(a) * TWO32 + (double)LO(a);
}

double QLG(__floatundidf)(u64 a)
{
	return (double)HI(a) * TWO32 + (double)LO(a);
}

/* To float: values of 2^53 and more first fold the bits below bit 11 into a
 * sticky bit (libgcc's method), so the double is exact and the final
 * conversion to float is the only rounding. */
float QLG(__floatdisf)(s64 a)
{
	u32 hi = HI(a), lo = LO(a);
	s32 shi = (s32)hi;

	if (shi >= (1 << 21) || shi < -(1 << 21)) { /* |a| >= 2^53, roughly */
		if (lo & 0x7ffu)
			lo = (lo & ~0x7ffu) | 0x800u;
	}
	return (float)((double)shi * TWO32 + (double)lo);
}

float QLG(__floatundisf)(u64 a)
{
	u32 hi = HI(a), lo = LO(a);

	if (hi >= (1u << 21)) /* a >= 2^53 */
		if (lo & 0x7ffu)
			lo = (lo & ~0x7ffu) | 0x800u;
	return (float)((double)hi * TWO32 + (double)lo);
}

/* ---- floating point -> integer (truncation) -------------------------------- */

static inline u32 d2u32(double a) /* 0 <= a < 2^32 */
{
	if (a >= 2147483648.0)
		return (u32)(s32)(a - 2147483648.0) + 0x80000000u;
	return (u32)(s32)a; /* fctiwz */
}

static u64 d2u64(double a)
{
	u32 hi, lo;

	if (!(a >= 1.0)) /* (-1, 1), NaN and negatives */
		return 0;
	if (a >= 18446744073709551616.0)
		return ~(u64)0;
	hi = d2u32(a * (1.0 / TWO32));             /* exact scaling */
	lo = d2u32(a - (double)hi * TWO32);       /* exact difference */
	return mk64(hi, lo);
}

static s64 d2s64(double a)
{
	if (a != a)
		return 0;
	if (a < 0.0) {
		if (a <= -9223372036854775808.0)
			return (s64)mk64(0x80000000u, 0);
		return (s64)neg64(d2u64(-a));
	}
	if (a >= 9223372036854775808.0)
		return (s64)mk64(0x7fffffffu, 0xffffffffu);
	return (s64)d2u64(a);
}

u64 QLG(__fixunsdfdi)(double a)
{
	return d2u64(a);
}

s64 QLG(__fixdfdi)(double a)
{
	return d2s64(a);
}

u64 QLG(__fixunssfdi)(float a)
{
	return d2u64((double)a);
}

s64 QLG(__fixsfdi)(float a)
{
	return d2s64((double)a);
}

u32 QLG(__fixunsdfsi)(double a)
{
	if (!(a >= 1.0))
		return 0;
	if (a >= TWO32)
		return 0xffffffffu;
	return d2u32(a);
}

u32 QLG(__fixunssfsi)(float a)
{
	return QLG(__fixunsdfsi)((double)a);
}

/* ---- powi: libgcc's algorithm, operation for operation -------------------- */

double QLG(__powidf2)(double x, int m)
{
	unsigned int n = m < 0 ? 0u - (unsigned int)m : (unsigned int)m;
	double y = (n & 1u) ? x : 1.0;

	while (n >>= 1) {
		x = x * x;
		if (n & 1u)
			y = y * x;
	}
	return m < 0 ? 1.0 / y : y;
}

float QLG(__powisf2)(float x, int m)
{
	unsigned int n = m < 0 ? 0u - (unsigned int)m : (unsigned int)m;
	float y = (n & 1u) ? x : 1.0f;

	while (n >>= 1) {
		x = x * x;
		if (n & 1u)
			y = y * x;
	}
	return m < 0 ? 1.0f / y : y;
}
