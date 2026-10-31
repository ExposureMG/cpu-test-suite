/* qemu stub for mathapx: int64 <-> double/float conversions, 64-bit
 * division / modulo, popcount / parity / clrsb on 64 bits, powi and sqrt:
 * all the libgcc helpers the 32-bit build needs.  APPROX (it mixes FP). */
#include "bench_kernel.h"

#define N 2048u

static void mathapx_init(void *arena)
{
	uint32_t *w = (uint32_t *)arena;
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0xa9a9u);
	for (i = 0; i < 2u * N; i++)
		w[i] = bench_rng_next(&r);
}

static double mathapx_run(void *arena)
{
	const uint32_t *w = (const uint32_t *)arena;
	double s = 0.0;
	uint64_t h = 0;
	uint32_t i;

	for (i = 0; i < N; i++) {
		uint64_t a = (uint64_t)w[2 * i] << 32 | w[2 * i + 1];
		uint64_t b = (a >> (w[2 * i] & 31u)) | 1u;
		int64_t sa = (int64_t)(a >> 1) - (int64_t)(b >> 2);
		double d = (double)sa * 1e-9;
		float f = (float)(int64_t)(a >> 20);

		h += a / b + a % (b | 3u) + (uint64_t)(sa / 7) + (uint64_t)(sa % 1000);
		h += (uint64_t)__builtin_popcountll(a) + (uint64_t)__builtin_parityll(b) +
		     (uint64_t)__builtin_clrsbll(sa);
		h += (uint64_t)(int64_t)(d * 1000.0) + (uint64_t)(f * 0.25f) + (uint64_t)(double)(a >> 11);
		s += __builtin_sqrt(d * d + 1.0) + __builtin_powi(1.0 + d * 1e-3, (int)(i & 15u)) +
		     (double)__builtin_sqrtf(f * 1e-12f + 1.0f);
	}
	return s + (double)(uint32_t)(h ^ (h >> 32)) * 1e-6;
}

BENCH_KDEF(mathapx) = {
	.name = "mathapx", .desc = "qemu stub: libgcc helpers", .unit = "eval",
	.category = BENCH_CAT_FP, .flags = BENCH_F_APPROX, .tolerance = 1e-12,
	.units = N, .arena_bytes = 2u * N * 4u,
	.init = mathapx_init, .run = mathapx_run,
};
