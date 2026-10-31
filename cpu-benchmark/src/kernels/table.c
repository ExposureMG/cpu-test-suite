/*
 * table.c - the kernel table and build description of one variant.
 *
 * Compiled once per variant, like the kernels.  The Makefile passes
 * BENCH_LABEL (toolchain label) and BENCH_CFLAGS (kernel flags) as strings.
 */
#include "bench_kernel.h"

#ifndef BENCH_LABEL
#define BENCH_LABEL BENCH_STR(BENCH_VARIANT)
#endif
#ifndef BENCH_CFLAGS
#define BENCH_CFLAGS "?"
#endif

#if defined(_ARCH_XENON) && defined(__VMX128__)
#define BENCH_CPU "xenon+vmx128"
#elif defined(_ARCH_XENON)
#define BENCH_CPU "xenon"
#elif defined(__PPU__)
#define BENCH_CPU "cell"
#elif defined(__powerpc__) || defined(__PPC__)
#define BENCH_CPU "ppc"
#else
#define BENCH_CPU "host"
#endif

#define BENCH_LIST(id) extern const bench_kernel BENCH_NS(k_##id);
#include "list_int.h"
#include "list_br.h"
#include "list_mem.h"
#include "list_fp.h"
#include "list_vec.h"
#include "list_mix.h"
#include "list_lib.h"
#undef BENCH_LIST

#define BENCH_LIST(id) &BENCH_NS(k_##id),
static const bench_kernel *const kernels[] = {
#include "list_int.h"
#include "list_br.h"
#include "list_mem.h"
#include "list_fp.h"
#include "list_vec.h"
#include "list_mix.h"
#include "list_lib.h"
	NULL
};
#undef BENCH_LIST

const bench_variant_info BENCH_NS(info) = {
	BENCH_STR(BENCH_VARIANT),
	BENCH_LABEL,
	"gcc " __VERSION__,
	BENCH_CPU,
	BENCH_CFLAGS,
	kernels,
};
