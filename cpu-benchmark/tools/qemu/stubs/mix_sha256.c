/* qemu stub for sha256: folds NATIVE 32-bit loads of a byte stream (as the
 * real kernel's "message words are big-endian, native" does), so the result
 * legitimately differs between big- and little-endian hosts: the BE/LE
 * cross-check should report it as DIFFERENT.  EXACT. */
#include "bench_kernel.h"

#define NB 16384u

static void sha256_init(void *arena)
{
	unsigned char *b = (unsigned char *)arena;
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0x5a256u);
	for (i = 0; i < NB; i++)
		b[i] = (unsigned char)bench_rng_next(&r);
}

static double sha256_run(void *arena)
{
	const uint32_t *w = (const uint32_t *)arena;
	uint32_t i, h = 0x6a09e667u;

	for (i = 0; i < NB / 4u; i++)
		h = bench_mix(h, (w[i] >> 7 | w[i] << 25) ^ w[i]);
	return bench_result_u32(h);
}

BENCH_KDEF(sha256) = {
	.name = "sha256", .desc = "qemu stub: native-endian words", .unit = "B",
	.category = BENCH_CAT_MIX, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = NB, .arena_bytes = NB,
	.init = sha256_init, .run = sha256_run,
};
