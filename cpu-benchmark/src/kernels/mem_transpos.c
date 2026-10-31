/*
 * mem_transpos.c - tiled 256x256 u32 matrix transpose (MEM).
 *
 * The source (256 KB) is transposed into a separate destination (256 KB) in
 * 16x16 tiles, the classic cache-blocked transpose used for texture
 * swizzling and FFT/DCT passes: inside a tile the loads walk half a source
 * line and the stores walk down a destination column at a 1 KB stride, so
 * every store lands in a different cache line.
 *
 * The fold reads every 7th element of every 7th row of the output (1369
 * samples; 7 is coprime with the tile size, so every position inside a tile
 * is sampled).  Sampling rows keeps the read-back to ~300 cache lines,
 * cheap next to the 65536 strided stores.
 */
#include "bench_kernel.h"

#define TR_N        256u
#define TR_TILE     16u
#define TR_BYTES    (TR_N * TR_N * 4u)
#define TR_SRC_OFF  0u
#define TR_DST_OFF  (TR_SRC_OFF + TR_BYTES)
#define ARENA_BYTES (TR_DST_OFF + TR_BYTES)
#define TR_SAMPLE   7u

static void transpos_init(void *arena)
{
	uint32_t *src = (uint32_t *)bench_at(arena, TR_SRC_OFF);
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0x7A115E5Du);
	for (i = 0; i < TR_N * TR_N; i++)
		src[i] = bench_rng_next(&rng);
}

/* dst = transpose(src), both TR_N x TR_N, row-major. */
static void tr_transpose(uint32_t *restrict dst, const uint32_t *restrict src)
{
	uint32_t ty, tx, y, x;

	for (ty = 0; ty < TR_N; ty += TR_TILE) {
		for (tx = 0; tx < TR_N; tx += TR_TILE) {
			for (y = ty; y < ty + TR_TILE; y++) {
				const uint32_t *s = src + y * TR_N + tx;
				uint32_t *d = dst + tx * TR_N + y;

				for (x = 0; x < TR_TILE; x++)
					d[x * TR_N] = s[x];
			}
		}
	}
}

static double transpos_run(void *arena)
{
	const uint32_t *src = (const uint32_t *)bench_at(arena, TR_SRC_OFF);
	uint32_t *dst = (uint32_t *)bench_at(arena, TR_DST_OFF);
	uint32_t h = 0, x, y;

	tr_transpose(dst, src);

	for (y = 0; y < TR_N; y += TR_SAMPLE) {
		const uint32_t *row = dst + y * TR_N;

		for (x = 0; x < TR_N; x += TR_SAMPLE)
			h = ((h << 5) | (h >> 27)) ^ row[x];
	}
	return bench_result_u32(bench_mix(h, TR_N));
}

BENCH_KDEF(transpos) = {
	.name = "transpos", .desc = "256x256 u32 transpose (16x16)", .unit = "elem",
	.category = BENCH_CAT_MEM, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = TR_N * TR_N, .arena_bytes = ARENA_BYTES,
	.init = transpos_init, .run = transpos_run,
};
