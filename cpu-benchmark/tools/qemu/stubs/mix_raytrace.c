/* qemu stub for raytrace: float sqrtf in a loop (GCC calls sqrtf on the G4)
 * and FP compares.  APPROX. */
#include "bench_kernel.h"

#define N 4096u

static void raytrace_init(void *arena)
{
	float *v = (float *)arena;
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0x7a7au);
	for (i = 0; i < 3u * N; i++)
		v[i] = bench_rng_f01(&r) * 2.0f - 1.0f;
}

static double raytrace_run(void *arena)
{
	const float *v = (const float *)arena;
	float acc = 0.0f;
	uint32_t i, hits = 0;

	for (i = 0; i < N; i++) {
		float x = v[3 * i], y = v[3 * i + 1], z = v[3 * i + 2];
		float d = x * x + y * y + z * z;

		if (d < 1.0f) {
			hits++;
			acc += __builtin_sqrtf(1.0f - d);
		} else {
			acc -= 1.0f / __builtin_sqrtf(d);
		}
	}
	return (double)acc + (double)hits;
}

BENCH_KDEF(raytrace) = {
	.name = "raytrace", .desc = "qemu stub: sqrtf", .unit = "ray",
	.category = BENCH_CAT_MIX, .flags = BENCH_F_APPROX, .tolerance = 1e-4,
	.units = N, .arena_bytes = 3u * N * 4u,
	.init = raytrace_init, .run = raytrace_run,
};
