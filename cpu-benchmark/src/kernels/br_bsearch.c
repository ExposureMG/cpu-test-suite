/*
 * br_bsearch.c - branchy lower_bound over a sorted u32 table (BR).
 *
 * A 65536-entry (256 KB) table of strictly increasing u32 keys, the kind of
 * sorted id / address table a game or emulator searches (symbol lookup,
 * sorted entity ids, block maps).  Gaps between neighbours are random in
 * 1..64.  4096 queries: even slots hold a key taken from the table, odd slots
 * a random value over (and slightly past) the key range, so about half of the
 * lookups hit.  Each query runs the classic lower_bound over [0, 65536):
 *
 *     if (a[mid] < key) lo = mid + 1; else hi = mid;
 *
 * and the returned indices (65536 for a key past the end) are folded with
 * bench_mix.
 */
#include "bench_kernel.h"

#define BS_N  65536u
#define BS_NQ 4096u /* DESIGN.md asks for 16384: 1.6 ms/run on the host, over rule 9 */

#define BS_TAB_OFF  0u
#define BS_Q_OFF    BENCH_ALIGN_UP(BS_TAB_OFF + BS_N * 4u, BENCH_LINE)
#define ARENA_BYTES BENCH_ALIGN_UP(BS_Q_OFF + BS_NQ * 4u, BENCH_LINE)

/* First index i in a[0..n) with a[i] >= key, n if there is none. */
static uint32_t bs_lower_bound(const uint32_t *a, uint32_t n, uint32_t key)
{
	uint32_t lo = 0, hi = n;

	while (lo < hi) {
		uint32_t mid = lo + ((hi - lo) >> 1);

		if (a[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void bsearch_init(void *arena)
{
	uint32_t *tab = (uint32_t *)bench_at(arena, BS_TAB_OFF);
	uint32_t *q = (uint32_t *)bench_at(arena, BS_Q_OFF);
	bench_rng rng;
	uint32_t i, v = 0;

	bench_rng_seed(&rng, 0xB5EA2C40u);
	for (i = 0; i < BS_N; i++) {
		v += 1u + bench_rng_below(&rng, 64u);
		tab[i] = v;
	}
	/* v is now the largest key; random probes go up to 1/32 past it. */
	for (i = 0; i < BS_NQ; i++) {
		if ((i & 1u) == 0)
			q[i] = tab[bench_rng_below(&rng, BS_N)];
		else
			q[i] = bench_rng_below(&rng, v + (v >> 5));
	}
}

static double bsearch_run(void *arena)
{
	const uint32_t *tab = (const uint32_t *)bench_at(arena, BS_TAB_OFF);
	const uint32_t *q = (const uint32_t *)bench_at(arena, BS_Q_OFF);
	uint32_t h = 0, i;

	for (i = 0; i < BS_NQ; i++)
		h = bench_mix(h, bs_lower_bound(tab, BS_N, q[i]));
	return bench_result_u32(h);
}

BENCH_KDEF(bsearch) = {
	.name = "bsearch", .desc = "lower_bound, 64K u32, 4K finds", .unit = "find",
	.category = BENCH_CAT_BR, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = BS_NQ, .arena_bytes = ARENA_BYTES,
	.init = bsearch_init, .run = bsearch_run,
};
