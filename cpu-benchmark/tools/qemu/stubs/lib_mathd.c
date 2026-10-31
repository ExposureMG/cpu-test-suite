/* qemu stub for mathd: every double BLIB() routine.  APPROX. */
#include "bench_kernel.h"
#include "bench_lib.h"

#define N 1024u

static void mathd_init(void *arena)
{
	double *x = (double *)arena;
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0xd0bu);
	for (i = 0; i < N; i++)
		x[i] = bench_rng_d01(&r) * 8.0 - 4.0;
}

static double mathd_run(void *arena)
{
	const double *x = (const double *)arena;
	double s = 0.0;
	uint32_t i;

	for (i = 0; i < N; i++) {
		double v = x[i];

		s += BLIB(sin)(v) + BLIB(cos)(v) + BLIB(exp)(v * 0.5) + BLIB(log)(v * v + 1.0) +
		     BLIB(sqrt)(v * v + 0.25);
	}
	return s;
}

BENCH_KDEF(mathd) = {
	.name = "mathd", .desc = "qemu stub: BLIB double math", .unit = "call",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_APPROX, .tolerance = 1e-9,
	.units = 5u * N, .arena_bytes = N * 8u,
	.init = mathd_init, .run = mathd_run,
};
