/*
 * variants.c - which kernel tables this image contains.
 *
 * Compiled with -DBENCH_A=<id> [-DBENCH_B=<id>]; <id>_info is the
 * bench_variant_info that src/kernels/table.c defines for each variant.
 *
 * Layout shifts (make SHIFTS=..., DESIGN.md section 1): the Makefile passes
 * -DBENCH_SHIFT_LIST='BENCH_SHIFT(0,) BENCH_SHIFT(4,_s4) ...', one entry per
 * shift with its byte offset and the suffix of the symbol namespace of the
 * copies linked at that shift (<id>_s4_info, ...; none at shift 0).  Without
 * it the image has the one copy at shift 0.
 */
#include <stddef.h>

#include "bench.h"
#include "runner.h"

#ifndef BENCH_A
#error "compile with -DBENCH_A=<variant id> [-DBENCH_B=<variant id>]"
#endif
#ifndef BENCH_SHIFT_LIST
#define BENCH_SHIFT_LIST BENCH_SHIFT(0, )
#endif

#define VINFO_(id, sfx) id##sfx##_info
#define VINFO(id, sfx) VINFO_(id, sfx)

#define BENCH_SHIFT(bytes, sfx) extern const bench_variant_info VINFO(BENCH_A, sfx);
BENCH_SHIFT_LIST
#undef BENCH_SHIFT
#ifdef BENCH_B
#define BENCH_SHIFT(bytes, sfx) extern const bench_variant_info VINFO(BENCH_B, sfx);
BENCH_SHIFT_LIST
#undef BENCH_SHIFT
#define VROW(sfx) { &VINFO(BENCH_A, sfx), &VINFO(BENCH_B, sfx) }
#else
#define VROW(sfx) { &VINFO(BENCH_A, sfx), NULL }
#endif

/* Row s: the variant tables of the copies linked at bench_shift_bytes[s]. */
const bench_variant_info *const bench_variants[][BENCH_MAX_VARIANTS] = {
#define BENCH_SHIFT(bytes, sfx) VROW(sfx),
	BENCH_SHIFT_LIST
#undef BENCH_SHIFT
};

const int bench_shift_bytes[] = {
#define BENCH_SHIFT(bytes, sfx) bytes,
	BENCH_SHIFT_LIST
#undef BENCH_SHIFT
};

const int bench_nshifts = sizeof(bench_shift_bytes) / sizeof(bench_shift_bytes[0]);

#ifdef BENCH_B
const int bench_nvariants = 2;
#else
const int bench_nvariants = 1;
#endif
