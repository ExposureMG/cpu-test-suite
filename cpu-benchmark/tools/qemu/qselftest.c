/*
 * qselftest.c - checks the qemu runtime's libgcc helpers and sqrt/sqrtf
 * through ordinary C operators.
 *
 * Compiled with the kernel flags by each Xenon compiler (-DQST_NS=xenon3,
 * -DQST_NS=old16), the operators below turn into calls to the helpers in
 * qlibgcc.c and qmath.c (64-bit division, popcount, int64<->FP conversions,
 * powi, sqrt...).  Compiled for the host (-DQST_NS=host), the same operators
 * are native x86-64 arithmetic.  Each group folds its results into a 64-bit
 * hash of values (not bytes), so the qemu output must equal the host output
 * line for line; tools/qemu/qcheck.sh compares them.
 *
 * Inputs avoid undefined behaviour (no division by zero, no INT64_MIN / -1,
 * conversions in range, clz/ctz of non-zero values).
 */
#include <stdint.h>

#include "qrt.h"

#ifndef QST_NS
#error "-DQST_NS=<tag>"
#endif

#define QST_PASTE_(a, b) a##_##b
#define QST_PASTE(a, b) QST_PASTE_(a, b)
#define QST_STR_(x) #x
#define QST_STR(x) QST_STR_(x)

#define QST_N 20000

typedef struct qst {
	uint64_t s;
} qst;

static uint64_t rnd64(qst *q) /* splitmix64 */
{
	uint64_t z = (q->s += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* Random value with a random bit width 1..64. */
static uint64_t rnd_wide(qst *q)
{
	uint64_t v = rnd64(q);
	unsigned w = (unsigned)(rnd64(q) & 63u) + 1u;

	return w == 64 ? v : v >> (64u - w);
}

static uint64_t mix(uint64_t h, uint64_t v)
{
	h ^= v;
	h *= 0x100000001B3ull;
	h ^= h >> 29;
	return h;
}

static uint64_t dbits(double d)
{
	union {
		double d;
		uint64_t u;
	} c;

	c.d = d;
	return c.u;
}

static uint32_t fbits(float f)
{
	union {
		float f;
		uint32_t u;
	} c;

	c.f = f;
	return c.u;
}

static double bitsd(uint64_t u)
{
	union {
		double d;
		uint64_t u;
	} c;

	c.u = u;
	return c.d;
}

static float bitsf(uint32_t u)
{
	union {
		float f;
		uint32_t u;
	} c;

	c.u = u;
	return c.f;
}

/* NaNs hash as one value (x86 and PowerPC default NaNs differ in sign). */
static uint64_t dkey(double d)
{
	return d != d ? 0x7ff8000000000000ull : dbits(d);
}

static uint64_t fkey(float f)
{
	return f != f ? 0x7fc00000u : fbits(f);
}

static void report(const char *name, uint64_t h)
{
	qrt_puts(QST_STR(QST_NS) " ");
	qrt_put_padded(name, 10);
	qrt_put_hex64(h);
	qrt_putc('\n');
}

void QST_PASTE(QST_NS, qselftest)(void);

void QST_PASTE(QST_NS, qselftest)(void)
{
	qst q;
	uint64_t h1, h2, h3;
	int i;

	/* unsigned 64-bit division and modulo */
	q.s = 1;
	h1 = h2 = 0;
	for (i = 0; i < QST_N; i++) {
		uint64_t a = rnd_wide(&q), b = rnd_wide(&q);

		if (!b)
			b = 1;
		h1 = mix(h1, a / b);
		h2 = mix(h2, a % b);
	}
	report("udiv64", h1);
	report("umod64", h2);

	/* signed 64-bit division and modulo */
	q.s = 2;
	h1 = h2 = 0;
	for (i = 0; i < QST_N; i++) {
		int64_t a = (int64_t)rnd_wide(&q), b = (int64_t)rnd_wide(&q);

		if (rnd64(&q) & 1)
			a = -(a & 0x7fffffffffffffffll);
		if (rnd64(&q) & 1)
			b = -(b & 0x7fffffffffffffffll);
		if (!b)
			b = 3;
		if (a == INT64_MIN && b == -1)
			b = 7;
		h1 = mix(h1, (uint64_t)(a / b));
		h2 = mix(h2, (uint64_t)(a % b));
	}
	report("sdiv64", h1);
	report("smod64", h2);

	/* shifts, multiply */
	q.s = 3;
	h1 = h2 = 0;
	for (i = 0; i < QST_N; i++) {
		uint64_t a = rnd64(&q), b = rnd_wide(&q);
		unsigned n = (unsigned)(rnd64(&q) & 63u);

		h1 = mix(h1, a << n);
		h1 = mix(h1, a >> n);
		h1 = mix(h1, (uint64_t)((int64_t)a >> n));
		h2 = mix(h2, a * b);
	}
	report("shift64", h1);
	report("mul64", h2);

	/* bit counting, 32 and 64 bits */
	q.s = 4;
	h1 = h2 = 0;
	for (i = 0; i < QST_N; i++) {
		uint64_t a = rnd_wide(&q);
		uint32_t x = (uint32_t)rnd_wide(&q);

		h1 = mix(h1, (uint64_t)__builtin_popcount(x));
		h1 = mix(h1, (uint64_t)__builtin_parity(x));
		h1 = mix(h1, (uint64_t)__builtin_clrsb((int32_t)x));
		h1 = mix(h1, (uint64_t)__builtin_ffs((int)x));
		h1 = mix(h1, __builtin_bswap32(x));
		if (x) {
			h1 = mix(h1, (uint64_t)__builtin_clz(x));
			h1 = mix(h1, (uint64_t)__builtin_ctz(x));
		}
		h2 = mix(h2, (uint64_t)__builtin_popcountll(a));
		h2 = mix(h2, (uint64_t)__builtin_parityll(a));
		h2 = mix(h2, (uint64_t)__builtin_clrsbll((int64_t)a));
		h2 = mix(h2, (uint64_t)__builtin_clrsbll(-(int64_t)(a >> 1)));
		h2 = mix(h2, (uint64_t)__builtin_ffsll((long long)a));
		h2 = mix(h2, __builtin_bswap64(a));
		if (a) {
			h2 = mix(h2, (uint64_t)__builtin_clzll(a));
			h2 = mix(h2, (uint64_t)__builtin_ctzll(a));
		}
	}
	report("bits32", h1);
	report("bits64", h2);

	/* integer -> floating point */
	q.s = 5;
	h1 = h2 = 0;
	for (i = 0; i < QST_N; i++) {
		uint64_t u = rnd_wide(&q);
		int64_t s = (rnd64(&q) & 1) ? -(int64_t)(u >> 1) : (int64_t)u;

		h1 = mix(h1, dbits((double)s));
		h1 = mix(h1, dbits((double)u));
		h2 = mix(h2, fbits((float)s));
		h2 = mix(h2, fbits((float)u));
	}
	report("i64tod", h1);
	report("i64tof", h2);

	/* floating point -> integer (in range) */
	q.s = 6;
	h1 = h2 = h3 = 0;
	for (i = 0; i < QST_N; i++) {
		uint64_t u = rnd_wide(&q);
		double scale = bitsd((uint64_t)(1023 - (rnd64(&q) % 24u)) << 52); /* 2^-0..-23 */
		double du = (double)u * scale;                       /* [0, 2^64) */
		double ds = (double)(int64_t)(u >> 1) * scale;       /* [0, 2^63) */
		float fu = (float)(u >> 1) * (float)scale;

		if (ds >= 9223372036854775808.0) /* (double) rounded up to 2^63 */
			ds = 0.0;
		if (rnd64(&q) & 1)
			ds = -ds;
		if (du >= 18446744073709551616.0)
			du = 0.0;
		h1 = mix(h1, (uint64_t)du);
		h1 = mix(h1, (uint64_t)(int64_t)ds);
		if (fu < 18446744073709551616.0f)
			h2 = mix(h2, (uint64_t)fu);
		h2 = mix(h2, (uint64_t)(int64_t)(fu * 0.5f));
		h2 = mix(h2, (uint64_t)(int64_t)(-fu * 0.5f));
		h3 = mix(h3, (uint32_t)(du * (1.0 / 4294967296.0)));
		h3 = mix(h3, (uint32_t)(float)(du * (1.0 / 8589934592.0)));
	}
	report("dtoi64", h1);
	report("ftoi64", h2);
	report("fptou32", h3);

	/* powi */
	q.s = 7;
	h1 = h2 = 0;
	for (i = 0; i < QST_N; i++) {
		double x = 0.5 + (double)(rnd64(&q) >> 44) * (1.5 / 1048576.0);
		int n = (int)(rnd64(&q) % 81u) - 40;

		h1 = mix(h1, dkey(__builtin_powi(x, n)));
		h2 = mix(h2, fkey(__builtin_powif((float)x, n)));
	}
	report("powidf2", h1);
	report("powisf2", h2);

	/* sqrt / sqrtf: random finite non-negative patterns, then specials */
	q.s = 8;
	h1 = h2 = 0;
	for (i = 0; i < QST_N; i++) {
		uint64_t b = rnd64(&q) & 0x7fffffffffffffffull;
		uint32_t c = (uint32_t)rnd64(&q) & 0x7fffffffu;

		if ((b >> 52) == 0x7ff)
			b &= 0x3fffffffffffffffull;
		if ((c >> 23) == 0xff)
			c &= 0x3fffffffu;
		if (i & 1) { /* half of them subnormal-ish or near 1 */
			b >>= (unsigned)(rnd64(&q) % 64u);
			c >>= (unsigned)(rnd64(&q) % 32u);
		}
		h1 = mix(h1, dkey(__builtin_sqrt(bitsd(b))));
		h2 = mix(h2, fkey(__builtin_sqrtf(bitsf(c))));
	}
	{
		static const uint64_t sd[] = {
			0x0000000000000000ull, 0x8000000000000000ull, 0x7ff0000000000000ull,
			0xfff0000000000000ull, 0x7ff8000000000000ull, 0x0000000000000001ull,
			0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
			0x3ff0000000000000ull, 0x4000000000000000ull, 0xbff0000000000000ull,
		};
		static const uint32_t sf[] = {
			0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
			0x00000001u, 0x007fffffu, 0x00800000u, 0x7f7fffffu, 0x3f800000u,
			0x40000000u, 0xbf800000u,
		};
		unsigned j;

		for (j = 0; j < sizeof sd / sizeof sd[0]; j++)
			h1 = mix(h1, dkey(__builtin_sqrt(bitsd(sd[j]))));
		for (j = 0; j < sizeof sf / sizeof sf[0]; j++)
			h2 = mix(h2, fkey(__builtin_sqrtf(bitsf(sf[j]))));
	}
	report("sqrt", h1);
	report("sqrtf", h2);
}
