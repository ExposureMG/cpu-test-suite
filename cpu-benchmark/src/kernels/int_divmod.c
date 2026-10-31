/*
 * int_divmod.c - integer division mix: number formatting, GCDs and the
 * divide/modulo-by-constant arithmetic of everyday code.
 *
 * One run does 18432 ops:
 *   - 8192 u32 -> decimal ASCII conversions (count the digits, then write
 *     them backwards two at a time with a 200-byte digit-pair table, so
 *     v / 100 and v % 100 by constants); the digits go to a scratch buffer
 *     in the arena and are folded back in;
 *   - 2048 GCDs by Euclid with '%' on variable divisors (divwu + mullw);
 *   - 8192 "mod triples": v % 7, v % 1000, v / 3 and the signed
 *     (-(v >> 1)) / 16, which must round toward zero.
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   DM_ITOA_OFF  uint32_t [8192]       32 KB  values to format
 *   DM_GCD_OFF   uint32_t [2048][2]    16 KB  GCD operand pairs
 *   DM_MOD_OFF   uint32_t [8192]       32 KB  values for the mod triples
 *   DM_TXT_OFF   char [128]                   text scratch (written by run)
 */
#include "bench_kernel.h"

#define DM_ITOA_N    8192u
#define DM_GCD_N     2048u
#define DM_MOD_N     8192u
#define DM_OPS       (DM_ITOA_N + DM_GCD_N + DM_MOD_N)

#define DM_ITOA_OFF  0u
#define DM_GCD_OFF   (DM_ITOA_OFF + DM_ITOA_N * 4u)
#define DM_MOD_OFF   (DM_GCD_OFF + DM_GCD_N * 8u)
#define DM_TXT_OFF   (DM_MOD_OFF + DM_MOD_N * 4u)
#define ARENA_BYTES  (DM_TXT_OFF + BENCH_LINE)

_Static_assert(DM_OPS == 18432u, "DESIGN.md: divmod does 18432 ops");

static const char dm_digits2[200] __attribute__((nonstring)) =
	"00010203040506070809"
	"10111213141516171819"
	"20212223242526272829"
	"30313233343536373839"
	"40414243444546474849"
	"50515253545556575859"
	"60616263646566676869"
	"70717273747576777879"
	"80818283848586878889"
	"90919293949596979899";

/* Number of decimal digits in v (1..10). */
static inline uint32_t dm_ndigits(uint32_t v)
{
	uint32_t n = 1;

	for (;;) {
		if (v < 10u)
			return n;
		if (v < 100u)
			return n + 1u;
		if (v < 1000u)
			return n + 2u;
		if (v < 10000u)
			return n + 3u;
		v /= 10000u;
		n += 4u;
	}
}

/* Writes v in decimal to out (no terminator) and returns the length. */
static uint32_t dm_utoa(char *out, uint32_t v)
{
	uint32_t n = dm_ndigits(v);
	char *p = out + n;

	while (v >= 100u) {
		uint32_t r = (v % 100u) * 2u;

		v /= 100u;
		*--p = dm_digits2[r + 1u];
		*--p = dm_digits2[r];
	}
	if (v >= 10u) {
		*--p = dm_digits2[v * 2u + 1u];
		*--p = dm_digits2[v * 2u];
	} else {
		*--p = (char)('0' + v);
	}
	return n;
}

static uint32_t dm_gcd(uint32_t a, uint32_t b)
{
	while (b != 0u) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static void divmod_init(void *arena)
{
	uint32_t *iv = bench_at(arena, DM_ITOA_OFF);
	uint32_t *gv = bench_at(arena, DM_GCD_OFF);
	uint32_t *mv = bench_at(arena, DM_MOD_OFF);
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0xD1D10001u);

	/* Magnitudes spread evenly over 1..32 bits: 1- to 10-digit numbers.
	 * (Two statements: the order of two rng calls in one expression is
	 * unspecified.) */
	for (i = 0; i < DM_ITOA_N; i++) {
		uint32_t x = bench_rng_next(&r);

		iv[i] = x >> bench_rng_below(&r, 32u);
	}

	/* Non-zero pairs sharing a random factor g <= 4096, so the GCDs vary;
	 * both cofactors are below 2^20, so g * x fits in 32 bits. */
	for (i = 0; i < DM_GCD_N; i++) {
		uint32_t g = 1u + bench_rng_below(&r, 4096u);

		gv[2 * i + 0] = g * (1u + bench_rng_below(&r, (1u << 20) - 1u));
		gv[2 * i + 1] = g * (1u + bench_rng_below(&r, (1u << 20) - 1u));
	}

	for (i = 0; i < DM_MOD_N; i++)
		mv[i] = bench_rng_next(&r);
}

static double divmod_run(void *arena)
{
	const uint32_t *iv = bench_at(arena, DM_ITOA_OFF);
	const uint32_t *gv = bench_at(arena, DM_GCD_OFF);
	const uint32_t *mv = bench_at(arena, DM_MOD_OFF);
	char *txt = bench_at(arena, DM_TXT_OFF);
	uint32_t h = 0, sa = 0, sb = 0, i, j;

	/* Formatting: fold every digit of every number. */
	for (i = 0; i < DM_ITOA_N; i++) {
		uint32_t n = dm_utoa(txt, iv[i]);
		uint32_t f = n;

		for (j = 0; j < n; j++)
			f = f * 31u + (unsigned char)txt[j];
		h = bench_mix(h, f);
	}

	for (i = 0; i < DM_GCD_N; i++)
		h = bench_mix(h, dm_gcd(gv[2 * i], gv[2 * i + 1]));

	for (i = 0; i < DM_MOD_N; i++) {
		uint32_t v = mv[i];
		uint32_t m7 = v % 7u;
		uint32_t m1000 = v % 1000u;
		uint32_t d3 = v / 3u;
		int32_t q16 = (-(int32_t)(v >> 1)) / 16;

		sa += m7 + m1000;
		sb = (sb ^ d3) + (uint32_t)q16;
	}
	h = bench_mix(h, sa);
	h = bench_mix(h, sb);
	return bench_result_u32(h);
}

BENCH_KDEF(divmod) = {
	.name = "divmod", .desc = "itoa, gcd, const div/mod mix", .unit = "op",
	.category = BENCH_CAT_INT, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = DM_OPS, .arena_bytes = ARENA_BYTES,
	.init = divmod_init, .run = divmod_run,
};
