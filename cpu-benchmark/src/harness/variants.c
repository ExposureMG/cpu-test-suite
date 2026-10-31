/*
 * variants.c - which kernel tables this image contains.
 *
 * Compiled with -DBENCH_A=<id> [-DBENCH_B=<id>]; <id>_info is the
 * bench_variant_info that src/kernels/table.c defines for each variant.
 */
#include <stddef.h>

#include "bench.h"

#ifndef BENCH_A
#error "compile with -DBENCH_A=<variant id> [-DBENCH_B=<variant id>]"
#endif

#define VINFO_(id) id##_info
#define VINFO(id) VINFO_(id)

extern const bench_variant_info VINFO(BENCH_A);
#ifdef BENCH_B
extern const bench_variant_info VINFO(BENCH_B);
#endif

const bench_variant_info *const bench_variants[] = {
	&VINFO(BENCH_A),
#ifdef BENCH_B
	&VINFO(BENCH_B),
#endif
	NULL,
};

const int bench_nvariants = sizeof(bench_variants) / sizeof(bench_variants[0]) - 1;
