/*
 * bench_kernel.h - include this (and only this) from kernel sources.
 *
 * Kernel translation units are compiled once per variant with
 * -DBENCH_VARIANT=<id>.  Every external symbol a kernel file defines must go
 * through BENCH_NS() / BENCH_KDEF() so the two variants can be linked into one
 * image; everything else in a kernel file is static.
 *
 * Rules for kernel code (checked at build time by tools/check_syms.py):
 *   - no mutable static or global data: everything mutable lives in the arena
 *     (static const tables are fine);
 *   - no calls into libc / libm except through BLIB() (see bench_lib.h) and the
 *     sqrt()/sqrtf() fallback calls GCC emits for errno handling;
 *   - init() derives all data from the bench_rng below using integer
 *     arithmetic, or exact conversions/scalings to float/double;
 *   - run() derives every pointer from the arena argument.
 */
#ifndef BENCH_KERNEL_H
#define BENCH_KERNEL_H

#include "bench.h"

#ifndef BENCH_VARIANT
#error "kernel sources must be compiled with -DBENCH_VARIANT=<id>"
#endif

#define BENCH_PASTE2_(a, b) a##_##b
#define BENCH_PASTE2(a, b) BENCH_PASTE2_(a, b)
#define BENCH_STR_(x) #x
#define BENCH_STR(x) BENCH_STR_(x)

/* BENCH_NS(foo) -> <variant>_foo */
#define BENCH_NS(name) BENCH_PASTE2(BENCH_VARIANT, name)

/* BENCH_KDEF(crc32) = { ... };  defines the descriptor <variant>_k_crc32 that
 * src/kernels/list_<cat>.h refers to as BENCH_LIST(crc32). */
#define BENCH_KDEF(id) const bench_kernel BENCH_NS(k_##id)

#define BENCH_ALIGN_UP(x, a) (((x) + ((a) - 1u)) & ~((a) - 1u))

/* Byte offset helper: (type *)bench_at(arena, off) */
static inline void *bench_at(void *arena, uint32_t off)
{
	return (unsigned char *)arena + off;
}

/* ---- deterministic PRNG for init() (xorshift32) ---------------------- */

typedef struct bench_rng {
	uint32_t s;
} bench_rng;

static inline void bench_rng_seed(bench_rng *r, uint32_t seed)
{
	r->s = seed ? seed : 0x9E3779B9u;
}

static inline uint32_t bench_rng_next(bench_rng *r)
{
	uint32_t x = r->s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	r->s = x;
	return x;
}

/* Uniform in [0, n). */
static inline uint32_t bench_rng_below(bench_rng *r, uint32_t n)
{
	return (uint32_t)(((uint64_t)bench_rng_next(r) * n) >> 32);
}

/* Uniform float in [0, 1), exact (24 random bits). */
static inline float bench_rng_f01(bench_rng *r)
{
	return (float)(bench_rng_next(r) >> 8) * (1.0f / 16777216.0f);
}

/* Uniform double in [0, 1), exact (53 random bits). */
static inline double bench_rng_d01(bench_rng *r)
{
	uint32_t hi = bench_rng_next(r) >> 5; /* 27 bits */
	uint32_t lo = bench_rng_next(r) >> 6; /* 26 bits */
	return ((double)hi * 67108864.0 + (double)lo) * (1.0 / 9007199254740992.0);
}

/* ---- result folding --------------------------------------------------- */

/* Order-dependent 32-bit mix, for folding integer outputs into a hash. */
static inline uint32_t bench_mix(uint32_t h, uint32_t v)
{
	h ^= v;
	h *= 0x9E3779B1u;
	h ^= h >> 15;
	return h;
}

/* An EXACT kernel's return value. */
static inline double bench_result_u32(uint32_t h)
{
	return (double)h;
}

/* ---- copy / fill without libc ----------------------------------------- *
 * GCC turns plain fill and strlen-style loops (and copies it can prove do
 * not overlap) into memset/strlen/memcpy calls, which kernels must not make.
 * Use these to reset working buffers inside run().  Both pointers must be
 * 4-byte aligned and bytes a multiple of 4.  They move the data as
 * bench_word, which may alias anything, so copying float or double data
 * through them is well defined and the compiler (whose IPA mod-ref analysis
 * sees through noinline) cannot assume they leave such data untouched. */

typedef uint32_t __attribute__((may_alias)) bench_word;

__attribute__((noinline, unused, optimize("no-tree-loop-distribute-patterns")))
static void bench_copy(void *dst, const void *src, uint32_t bytes)
{
	bench_word *d = (bench_word *)dst;
	const bench_word *s = (const bench_word *)src;
	uint32_t i, n = bytes >> 2;

	for (i = 0; i < n; i++)
		d[i] = s[i];
}

__attribute__((noinline, unused, optimize("no-tree-loop-distribute-patterns")))
static void bench_fill(void *dst, uint32_t value, uint32_t bytes)
{
	bench_word *d = (bench_word *)dst;
	uint32_t i, n = bytes >> 2;

	for (i = 0; i < n; i++)
		d[i] = value;
}

#endif /* BENCH_KERNEL_H */
