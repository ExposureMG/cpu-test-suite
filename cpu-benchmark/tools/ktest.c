/*
 * ktest.c - host-side smoke test for a single kernel (used by tools/kcheck.sh).
 *
 * Built with -DBENCH_HOST -DBENCH_VARIANT=kt -DKTEST_ID=<id> together with the
 * kernel's source file.  Runs init() once and run() several times, checks that
 * every run returns the same value, and prints the value and the time per run.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench_kernel.h"

#ifndef KTEST_ID
#error "-DKTEST_ID=<kernel id>"
#endif

#define KTEST_DESC_(id) BENCH_NS(k_##id)
#define KTEST_DESC(id) KTEST_DESC_(id)

extern const bench_kernel KTEST_DESC(KTEST_ID);

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(void)
{
	const bench_kernel *k = &KTEST_DESC(KTEST_ID);
	const char *cats[] = { "INT", "BR", "MEM", "FP", "VEC", "MIX", "LIB" };
	void *raw = malloc(BENCH_ARENA_BYTES + BENCH_ARENA_ALIGN);
	unsigned char *arena;
	double first, v, t0, best = 1e30;
	int i, bad = 0;

	if (!raw)
		return 2;
	arena = (unsigned char *)(((uintptr_t)raw + BENCH_ARENA_ALIGN - 1) &
				  ~(uintptr_t)(BENCH_ARENA_ALIGN - 1));
	/* Poison so that kernels relying on zeroed memory are caught. */
	memset(arena, 0xA5, BENCH_ARENA_BYTES);

	if (!k->name || strlen(k->name) > 8 || !k->desc || strlen(k->desc) > 32 ||
	    !k->unit || strlen(k->unit) > 4 || k->category >= BENCH_CAT_COUNT ||
	    !k->units || !k->arena_bytes || k->arena_bytes > BENCH_ARENA_BYTES ||
	    !k->init || !k->run ||
	    (k->flags == BENCH_F_APPROX && !(k->tolerance > 0.0)) ||
	    k->flags > BENCH_F_APPROX) {
		printf("FAIL %s: bad descriptor\n", k->name ? k->name : "?");
		return 1;
	}

	k->init(arena);
	first = k->run(arena);
	for (i = 0; i < 5; i++) {
		t0 = now_us();
		v = k->run(arena);
		t0 = now_us() - t0;
		if (t0 < best)
			best = t0;
		if (memcmp(&v, &first, sizeof v) != 0)
			bad++;
	}
	if (k->flags == BENCH_F_EXACT && (first != (double)(uint32_t)first))
		bad++;

	printf("%s %-8s %-3s %-32s result=%.17g (0x%08x) host=%.1f us/run "
	       "(%.3f ns/%s) arena=%u KB\n",
	       bad ? "FAIL" : "ok  ", k->name, cats[k->category], k->desc, first,
	       k->flags == BENCH_F_EXACT ? (unsigned)(uint32_t)first : 0u, best,
	       best * 1e3 / k->units, k->unit, k->arena_bytes / 1024u);
	if (bad)
		printf("FAIL %s: run() is not idempotent or an EXACT result is not a "
		       "uint32\n", k->name);
	free(raw);
	return bad ? 1 : 0;
}
