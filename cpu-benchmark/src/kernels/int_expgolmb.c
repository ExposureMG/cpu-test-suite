/*
 * int_expgolmb.c - Exp-Golomb ue(v) decoding, as in an H.264 slice-header
 * or CAVLC syntax-element reader.
 *
 * ue(v) codes x = v + 1 as n zero bits followed by the n + 1 bits of x, where
 * n = floor(log2(x)).  init writes 65536 codes MSB-first into a byte buffer;
 * run reads them back with a 64-bit bit reservoir: the next code sits in the
 * top bits, its length comes from a leading-zero count, the value from a
 * variable right shift, and the reservoir is refilled 32 bits at a time
 * (one big-endian word assembled from bytes, always 4-byte aligned) whenever
 * fewer than EG_MAXLEN bits are left.
 *
 * The values are skewed towards small numbers like real syntax elements.
 * Each length class n is used a fixed number of times (EG_CLASSES), in random
 * order, so the stream length is a compile-time constant.  v < 4096, so the
 * longest code is ue(4095): 12 zeros + 13 bits = 25 bits.
 *
 * The reader never needs more than 56 bits beyond the last code, so it reads
 * at most 8 bytes past the end of the stream; the stream is followed by at
 * least EG_PAD zero bytes (64 here) inside the kernel's arena.
 *
 * Arena layout:
 *   EG_BUF_OFF  stream (EG_STREAM_BYTES) + zero padding, EG_BUF_BYTES total
 */
#include "bench_kernel.h"

#define EG_SYMS     65536u
#define EG_VMAX     4095u   /* largest value coded */
#define EG_MAXLEN   25u     /* bits in ue(EG_VMAX) */

/* X(n, count): count codes of length class n, i.e. v in [2^n - 1, 2^(n+1) - 2]
 * (class 12 holds only v = 4095), each 2n + 1 bits long. */
#define EG_CLASSES(X) \
	X(0, 16384) X(1, 12288) X(2, 9216) X(3, 7168) X(4, 5632) \
	X(5, 4352) X(6, 3328) X(7, 2560) X(8, 1792) X(9, 1280) \
	X(10, 896) X(11, 512) X(12, 128)
#define EG_NCLASS   13u

#define EG_COUNT_(n, c)  c,
#define EG_SUM_(n, c)    + (c)
#define EG_BITS_(n, c)   + (c) * (2u * (n) + 1u)

#define EG_STREAM_BITS  (0u EG_CLASSES(EG_BITS_))
#define EG_STREAM_BYTES ((EG_STREAM_BITS + 7u) / 8u)
#define EG_PAD          16u
#define EG_BUF_OFF      0u
#define EG_BUF_BYTES    BENCH_ALIGN_UP(EG_STREAM_BYTES + EG_PAD, BENCH_LINE)
#define ARENA_BYTES     (EG_BUF_OFF + EG_BUF_BYTES)

_Static_assert((0u EG_CLASSES(EG_SUM_)) == EG_SYMS, "class counts must sum to EG_SYMS");

static const uint16_t eg_count[EG_NCLASS] = { EG_CLASSES(EG_COUNT_) };

static void expgolmb_init(void *arena)
{
	unsigned char *buf = bench_at(arena, EG_BUF_OFF);
	unsigned char *out = buf;
	uint32_t left[EG_NCLASS];
	uint32_t remaining = EG_SYMS;
	uint32_t acc = 0;   /* pending bits, right-aligned */
	uint32_t nacc = 0;  /* number of pending bits, < 8 between codes */
	bench_rng r;
	uint32_t i, n;

	/* Zero the buffer first: that is the padding after the stream. */
	bench_fill(buf, 0, EG_BUF_BYTES);

	for (n = 0; n < EG_NCLASS; n++)
		left[n] = eg_count[n];

	bench_rng_seed(&r, 0xE6E60001u);
	for (i = 0; i < EG_SYMS; i++) {
		uint32_t u = bench_rng_below(&r, remaining);
		uint32_t x, len;

		/* Draw a length class without replacement. */
		for (n = 0; u >= left[n]; n++)
			u -= left[n];
		left[n]--;
		remaining--;

		/* x = v + 1 in [2^n, 2^(n+1)), capped so that v <= EG_VMAX. */
		x = (1u << n) | (bench_rng_next(&r) & ((1u << n) - 1u));
		if (x > EG_VMAX + 1u)
			x = EG_VMAX + 1u;

		/* n zeros then the n + 1 bits of x: x itself in 2n + 1 bits. */
		len = 2u * n + 1u;
		acc = (acc << len) | x;
		nacc += len;
		while (nacc >= 8u) {
			nacc -= 8u;
			*out++ = (unsigned char)(acc >> nacc);
		}
		acc &= (1u << nacc) - 1u;
	}
	if (nacc)
		*out = (unsigned char)(acc << (8u - nacc));
}

static double expgolmb_run(void *arena)
{
	const unsigned char *p = bench_at(arena, EG_BUF_OFF);
	uint64_t cache = 0;  /* bit reservoir, next bit in bit 63 */
	uint32_t bits = 0;   /* valid bits in cache */
	uint32_t s1 = 0, s2 = 0;
	uint32_t i;

	for (i = 0; i < EG_SYMS; i++) {
		uint32_t lz, len, v;

		if (bits < EG_MAXLEN) {
			uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
				     (uint32_t)p[2] << 8 | (uint32_t)p[3];

			cache |= (uint64_t)w << (32u - bits);
			bits += 32u;
			p += 4;
		}

		/* The top EG_MAXLEN bits hold a whole code, so cache != 0. */
		lz = (uint32_t)__builtin_clzll(cache);
		len = 2u * lz + 1u;
		v = (uint32_t)(cache >> (64u - len)) - 1u;
		cache <<= len;
		bits -= len;

		/* Fletcher-style fold: order-sensitive and cheap. */
		s1 += v;
		s2 += s1;
	}
	return bench_result_u32(bench_mix(bench_mix(0, s1), s2));
}

BENCH_KDEF(expgolmb) = {
	.name = "expgolmb", .desc = "Exp-Golomb ue(v) decode, 64K", .unit = "sym",
	.category = BENCH_CAT_INT, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = EG_SYMS, .arena_bytes = ARENA_BYTES,
	.init = expgolmb_init, .run = expgolmb_run,
};
