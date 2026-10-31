/*
 * lib_qsortlib.c - the toolchain's newlib qsort() with a kernel comparator (LIB).
 *
 * 4096 random u32 are copied into a work buffer (bench_copy) and sorted with
 * BLIB(qsort), i.e. the qsort that each variant's newlib build provides.  The
 * comparator is the usual overflow-safe "(a > b) - (a < b)" and lives in this
 * file, so it is compiled by the variant's own compiler; qsort calls it
 * through a function pointer for every comparison.  The sorted array is folded
 * with the order-dependent bench_mix, so the result also proves the order.
 */
#include "bench_kernel.h"
#include "bench_lib.h"

#define QL_N 4096u

#define QL_IN_OFF   0u
#define QL_WORK_OFF BENCH_ALIGN_UP(QL_IN_OFF + QL_N * 4u, BENCH_LINE)
#define ARENA_BYTES BENCH_ALIGN_UP(QL_WORK_OFF + QL_N * 4u, BENCH_LINE)

static int u32_cmp(const void *pa, const void *pb)
{
	uint32_t a = *(const uint32_t *)pa;
	uint32_t b = *(const uint32_t *)pb;

	return (a > b) - (a < b);
}

static void qsortlib_init(void *arena)
{
	uint32_t *in = (uint32_t *)bench_at(arena, QL_IN_OFF);
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0x0DDC0DE5u);
	for (i = 0; i < QL_N; i++)
		in[i] = bench_rng_next(&rng);
}

static double qsortlib_run(void *arena)
{
	const uint32_t *in = (const uint32_t *)bench_at(arena, QL_IN_OFF);
	uint32_t *a = (uint32_t *)bench_at(arena, QL_WORK_OFF);
	uint32_t h = 0, i;

	bench_copy(a, in, QL_N * 4u);
	BLIB(qsort)(a, QL_N, sizeof(uint32_t), u32_cmp);

	for (i = 0; i < QL_N; i++)
		h = bench_mix(h, a[i]);
	return bench_result_u32(h);
}

BENCH_KDEF(qsortlib) = {
	.name = "qsortlib", .desc = "newlib qsort, 4096 u32", .unit = "elem",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = QL_N, .arena_bytes = ARENA_BYTES,
	.init = qsortlib_init, .run = qsortlib_run,
};
