/*
 * mem_stream.c - STREAM triad a[i] = b[i] + s * c[i], double (MEM).
 *
 * Three arrays of 262144 doubles, 2 MB each (6 MB, six times the L2): main
 * memory bandwidth with a simple FP body.  The scalar is read from the
 * arena, so the compiler cannot treat it as a constant.  units = the bytes
 * STREAM counts (read b and c, write a: 24 B per element).
 *
 * The triad is processed in blocks of 64 elements, and after each block its
 * first element is read back and summed (4096 samples).  Reading the
 * samples in a second pass would cost ~3400 serialised DRAM misses on the
 * in-order core (most of a[] has left the L2 by then); a line written a few
 * hundred cycles earlier is still close.
 *
 * APPROX 1e-12: with or without FMA contraction (fmadd versus fmul + fadd)
 * each a[i] differs by at most 3 * 2^-53 relative (all terms are positive,
 * so there is no cancellation).  Both variants sum the same 4096 positive
 * samples in the same order, so even if every partial sum rounded the other
 * way the sums differ by at most (2 * 4095 + 3) * 2^-53 ~= 9.1e-13 relative;
 * typical differences are ~1e-15.  A single wrong sample (they average 2.0
 * of a ~8200 total) moves the sum by far more than 1e-12.
 */
#include "bench_kernel.h"

#define ST_N        262144u
#define ST_BLOCK    64u
#define ST_BYTES    (ST_N * 8u)
#define ST_A_OFF    0u
#define ST_B_OFF    (ST_A_OFF + ST_BYTES)
#define ST_C_OFF    (ST_B_OFF + ST_BYTES)
#define ST_S_OFF    (ST_C_OFF + ST_BYTES)
#define ARENA_BYTES (ST_S_OFF + BENCH_LINE)

static void stream_init(void *arena)
{
	double *b = (double *)bench_at(arena, ST_B_OFF);
	double *c = (double *)bench_at(arena, ST_C_OFF);
	double *s = (double *)bench_at(arena, ST_S_OFF);
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0x57EA3001u);
	for (i = 0; i < ST_N; i++) {
		b[i] = bench_rng_d01(&rng);
		c[i] = bench_rng_d01(&rng);
	}
	*s = 3.0;	/* the STREAM default scalar */
}

/* a = b + s * c; returns the sum of the first element of every block. */
static double st_triad(double *restrict a, const double *restrict b,
		       const double *restrict c, double s)
{
	double sum = 0.0;
	uint32_t i, j;

	for (i = 0; i < ST_N; i += ST_BLOCK) {
		for (j = i; j < i + ST_BLOCK; j++)
			a[j] = b[j] + s * c[j];
		sum += a[i];
	}
	return sum;
}

static double stream_run(void *arena)
{
	return st_triad((double *)bench_at(arena, ST_A_OFF),
			(const double *)bench_at(arena, ST_B_OFF),
			(const double *)bench_at(arena, ST_C_OFF),
			*(const double *)bench_at(arena, ST_S_OFF));
}

BENCH_KDEF(stream) = {
	.name = "stream", .desc = "STREAM triad, 3 x 2 MB double", .unit = "B",
	.category = BENCH_CAT_MEM, .flags = BENCH_F_APPROX, .tolerance = 1e-12,
	.units = 3u * ST_BYTES, .arena_bytes = ARENA_BYTES,
	.init = stream_init, .run = stream_run,
};
