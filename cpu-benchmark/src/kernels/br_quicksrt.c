/*
 * br_quicksrt.c - hand-written quicksort of 8192 int32 (BR).
 *
 * The classic textbook / game-engine quicksort: median-of-3 pivot (the three
 * samples are sorted in place and act as sentinels), Hoare-style partition
 * that stops on keys equal to the pivot (so runs of duplicates split evenly),
 * insertion sort for partitions of 16 or fewer keys, and recursion on the
 * smaller side only while the larger side loops, which bounds the recursion
 * depth to log2(n / 16).
 *
 * Input: 8192 int32, about one quarter drawn from [-2048, 2048) so there are
 * plenty of duplicates, the rest spread over [-2^30, 2^30).  run() copies the
 * input into a work buffer (bench_copy), sorts it, and folds the sorted array
 * with the order-dependent bench_mix, so the result also proves the order.
 */
#include "bench_kernel.h"

#define QS_N      8192u
#define QS_CUTOFF 16

#define QS_IN_OFF   0u
#define QS_WORK_OFF BENCH_ALIGN_UP(QS_IN_OFF + QS_N * 4u, BENCH_LINE)
#define ARENA_BYTES BENCH_ALIGN_UP(QS_WORK_OFF + QS_N * 4u, BENCH_LINE)

#define QS_SWAP(x, y) do { int32_t t_ = (x); (x) = (y); (y) = t_; } while (0)

/* Straight insertion sort of a[lo..hi] (inclusive). */
static void qs_insertion(int32_t *a, int lo, int hi)
{
	int i, j;

	for (i = lo + 1; i <= hi; i++) {
		int32_t v = a[i];

		for (j = i - 1; j >= lo && a[j] > v; j--)
			a[j + 1] = a[j];
		a[j + 1] = v;
	}
}

/* Sort a[lo..hi] (inclusive). */
static void qs_sort(int32_t *a, int lo, int hi)
{
	while (hi - lo + 1 > QS_CUTOFF) {
		int mid = lo + ((hi - lo) >> 1);
		int i, j;
		int32_t pivot;

		/* Median of three: afterwards a[lo] <= a[mid] <= a[hi]. */
		if (a[mid] < a[lo])
			QS_SWAP(a[mid], a[lo]);
		if (a[hi] < a[lo])
			QS_SWAP(a[hi], a[lo]);
		if (a[hi] < a[mid])
			QS_SWAP(a[hi], a[mid]);

		/* Park the pivot at hi - 1; a[lo] and a[hi - 1] are the
		 * sentinels that stop the two scans. */
		pivot = a[mid];
		QS_SWAP(a[mid], a[hi - 1]);
		i = lo;
		j = hi - 1;
		for (;;) {
			while (a[++i] < pivot)
				;
			while (pivot < a[--j])
				;
			if (i >= j)
				break;
			QS_SWAP(a[i], a[j]);
		}
		QS_SWAP(a[i], a[hi - 1]);

		/* a[lo..i-1] <= pivot == a[i] <= a[i+1..hi].  Recurse into the
		 * smaller side, keep looping on the larger one. */
		if (i - lo < hi - i) {
			qs_sort(a, lo, i - 1);
			lo = i + 1;
		} else {
			qs_sort(a, i + 1, hi);
			hi = i - 1;
		}
	}
	qs_insertion(a, lo, hi);
}

static void quicksrt_init(void *arena)
{
	uint32_t *in = (uint32_t *)bench_at(arena, QS_IN_OFF);
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0x51C0FFEEu);
	for (i = 0; i < QS_N; i++) {
		uint32_t r = bench_rng_next(&rng);
		int32_t v;

		if ((r >> 30) == 0)  /* 1 in 4: small range, many duplicates */
			v = (int32_t)(r & 4095u) - 2048;
		else
			v = (int32_t)(r & 0x7FFFFFFFu) - 0x40000000;
		in[i] = (uint32_t)v;
	}
}

static double quicksrt_run(void *arena)
{
	const int32_t *in = (const int32_t *)bench_at(arena, QS_IN_OFF);
	int32_t *a = (int32_t *)bench_at(arena, QS_WORK_OFF);
	uint32_t h = 0, i;

	bench_copy(a, in, QS_N * 4u);
	qs_sort(a, 0, (int)QS_N - 1);

	for (i = 0; i < QS_N; i++)
		h = bench_mix(h, (uint32_t)a[i]);
	return bench_result_u32(h);
}

BENCH_KDEF(quicksrt) = {
	.name = "quicksrt", .desc = "quicksort 8192 int32, med-of-3", .unit = "elem",
	.category = BENCH_CAT_BR, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = QS_N, .arena_bytes = ARENA_BYTES,
	.init = quicksrt_init, .run = quicksrt_run,
};
