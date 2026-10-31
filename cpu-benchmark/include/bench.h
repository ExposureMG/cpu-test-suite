/*
 * bench.h - types shared by the harness and every kernel variant.
 *
 * The same kernel sources are compiled once per toolchain ("variant").  Each
 * variant exports a bench_variant_info whose kernel table lists the kernels in
 * an identical order, so the harness can run kernel i of variant A and kernel i
 * of variant B back to back.  Everything in this header must have the same
 * layout under every compiler that builds a variant (plain C types only).
 */
#ifndef BENCH_H
#define BENCH_H

#include <stddef.h>
#include <stdint.h>

#define BENCH_VERSION "1.0"

/* One arena is shared by all kernels (and by both variants of a kernel, so
 * both see the same addresses).  The harness aligns it to BENCH_ARENA_ALIGN. */
#define BENCH_ARENA_BYTES (32u * 1024u * 1024u)
#define BENCH_ARENA_ALIGN 65536u

/* Cache line size of the Xenon L1/L2. */
#define BENCH_LINE 128u

typedef enum bench_category {
	BENCH_CAT_INT = 0, /* integer ALU, bit manipulation, multiply / divide   */
	BENCH_CAT_BR,      /* branch-heavy control flow                          */
	BENCH_CAT_MEM,     /* memory hierarchy: copies, strides, latency, stream */
	BENCH_CAT_FP,      /* scalar floating point                              */
	BENCH_CAT_VEC,     /* SIMD: AltiVec intrinsics, auto-vectorisable loops  */
	BENCH_CAT_MIX,     /* application-style workloads (crypto, codecs, ...)  */
	BENCH_CAT_LIB,     /* the toolchain's own newlib libc / libm routines    */
	BENCH_CAT_COUNT
} bench_category;

/* Result comparison between variants. */
#define BENCH_F_EXACT  0u /* integer result: must be bit-identical           */
#define BENCH_F_APPROX 1u /* FP result: |a-b| <= tolerance * max(|a|,|b|)    */

typedef struct bench_kernel {
	const char *name;      /* unique id, [a-z0-9], <= 8 chars ("crc32")      */
	const char *desc;      /* <= 32 chars, shown on wide screens / serial    */
	const char *unit;      /* work unit, <= 4 chars ("B", "elem", "px")      */
	uint32_t category;     /* bench_category                                 */
	uint32_t flags;        /* BENCH_F_*                                      */
	double tolerance;      /* relative tolerance, BENCH_F_APPROX only        */
	uint32_t units;        /* work units processed by one run()              */
	uint32_t arena_bytes;  /* arena bytes the kernel touches                 */
	/* Builds the inputs in the arena.  Deterministic.  Called once per
	 * kernel, from ONE variant only; the other variant reuses the data. */
	void (*init)(void *arena);
	/* One pass over the workload.  Must not modify its inputs, must return
	 * the same value on every call, and must keep no state outside the
	 * arena.  EXACT kernels return (double)<uint32 hash>. */
	double (*run)(void *arena);
} bench_kernel;

typedef struct bench_variant_info {
	const char *id;        /* symbol namespace, e.g. "xenon3"                */
	const char *label;     /* toolchain label, e.g. "testxenon3"             */
	const char *compiler;  /* "gcc 16.2.0"                                   */
	const char *cpu;       /* "xenon+vmx128", "cell", "host"                 */
	const char *cflags;    /* optimisation flags the kernels were built with */
	const bench_kernel *const *kernels; /* NULL-terminated, fixed order      */
} bench_variant_info;

#endif /* BENCH_H */
