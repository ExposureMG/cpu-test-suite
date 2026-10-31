/*
 * int_bitops.c - 64-bit bit manipulation over 8192 random words, the kind of
 * helpers a bitboard engine, an allocator bitmap or a CPU emulator's flag
 * logic is built from.
 *
 * Per word: population count, count leading/trailing zeros (__builtin_clzll
 * and __builtin_ctzll on inputs forced non-zero), parity, a full 64-bit bit
 * reversal (mask-and-shift swap ladder), a rotate by a data-dependent amount
 * (1..63), and a single-bit test at a data-dependent position that picks one
 * of two updates: an unpredictable branch, taken for about 45 % of the
 * random words (bit k of w with k = w & 63 is always 0 when k < 6).
 *
 * Xenon has no popcount or parity instruction, and GCC lowers
 * __builtin_popcountll / __builtin_parityll to the libgcc calls
 * __popcountdi2 / __paritydi2 there, which kernels may not make (and which
 * the A/B image would take from one toolchain's libgcc for both variants).
 * So popcount and parity are the classic SWAR versions, as PowerPC game code
 * of the era wrote them.
 *
 * Arena layout:
 *   BO_SRC_OFF  uint64_t [8192]  64 KB  (read-only in run)
 */
#include "bench_kernel.h"

#define BO_WORDS    8192u
#define BO_SRC_OFF  0u
#define ARENA_BYTES (BO_SRC_OFF + BO_WORDS * 8u)

static inline uint32_t bo_popcount64(uint64_t w)
{
	w -= (w >> 1) & 0x5555555555555555ull;
	w = (w & 0x3333333333333333ull) + ((w >> 2) & 0x3333333333333333ull);
	w = (w + (w >> 4)) & 0x0F0F0F0F0F0F0F0Full;
	return (uint32_t)((w * 0x0101010101010101ull) >> 56);
}

static inline uint32_t bo_parity64(uint64_t w)
{
	uint32_t x = (uint32_t)w ^ (uint32_t)(w >> 32);

	x ^= x >> 16;
	x ^= x >> 8;
	x ^= x >> 4;
	return (0x6996u >> (x & 15u)) & 1u;
}

static inline uint64_t bo_bitrev64(uint64_t w)
{
	w = ((w >> 1) & 0x5555555555555555ull) | ((w & 0x5555555555555555ull) << 1);
	w = ((w >> 2) & 0x3333333333333333ull) | ((w & 0x3333333333333333ull) << 2);
	w = ((w >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((w & 0x0F0F0F0F0F0F0F0Full) << 4);
	w = ((w >> 8) & 0x00FF00FF00FF00FFull) | ((w & 0x00FF00FF00FF00FFull) << 8);
	w = ((w >> 16) & 0x0000FFFF0000FFFFull) | ((w & 0x0000FFFF0000FFFFull) << 16);
	return (w >> 32) | (w << 32);
}

/* r must be in 1..63. */
static inline uint64_t bo_rotl64(uint64_t w, uint32_t r)
{
	return (w << r) | (w >> (64u - r));
}

static void bitops_init(void *arena)
{
	uint64_t *src = bench_at(arena, BO_SRC_OFF);
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0xB0B00001u);
	for (i = 0; i < BO_WORDS; i++) {
		uint64_t hi = bench_rng_next(&r);
		uint64_t lo = bench_rng_next(&r);

		src[i] = hi << 32 | lo;
	}
}

static double bitops_run(void *arena)
{
	const uint64_t *src = bench_at(arena, BO_SRC_OFF);
	uint32_t pop = 0, lz = 0, tz = 0, par = 0, hits = 0, h, i;
	uint64_t acc = 0, alt = 0;

	for (i = 0; i < BO_WORDS; i++) {
		uint64_t w = src[i];
		uint32_t k = (uint32_t)w & 63u;

		pop += bo_popcount64(w);
		lz += (uint32_t)__builtin_clzll(w | 1u);
		tz += (uint32_t)__builtin_ctzll(w | (1ull << 63));
		par += bo_parity64(w);
		acc ^= bo_bitrev64(w) + bo_rotl64(w, (uint32_t)(w >> 58) | 1u);

		if (w & (1ull << k)) {
			hits += k;
			alt += w >> k;
		} else {
			alt ^= w;
		}
	}

	h = bench_mix(0, pop);
	h = bench_mix(h, lz);
	h = bench_mix(h, tz);
	h = bench_mix(h, par);
	h = bench_mix(h, hits);
	h = bench_mix(h, (uint32_t)acc);
	h = bench_mix(h, (uint32_t)(acc >> 32));
	h = bench_mix(h, (uint32_t)alt);
	h = bench_mix(h, (uint32_t)(alt >> 32));
	return bench_result_u32(h);
}

BENCH_KDEF(bitops) = {
	.name = "bitops", .desc = "popcnt/clz/ctz/rev/rot, 8K u64", .unit = "word",
	.category = BENCH_CAT_INT, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = BO_WORDS, .arena_bytes = ARENA_BYTES,
	.init = bitops_init, .run = bitops_run,
};
