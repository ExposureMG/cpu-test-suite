/*
 * lib_memcpy.c - the toolchain's newlib memcpy (LIB).
 *
 * A 128 KB random source region and a separate 128 KB destination region
 * (they never overlap).  One run():
 *
 *   1. 4 aligned 32 KB copies: dst = src with its four 32 KB chunks rotated
 *      by one.  They cover all of dst, so they also reset it: whatever the
 *      previous run (of either variant) left there is overwritten.
 *   2. 4096 small copies, lengths 1..256 (each length exactly 16 times, in
 *      shuffled order) from arbitrary byte offsets of src to arbitrary byte
 *      offsets of dst, so source and destination alignment vary
 *      independently, like a game patching sprites, strings and packets.
 *      Offsets and lengths are precomputed by init.
 *   3. Fold a sample of dst: the 4 bytes at every 244th offset (one byte
 *      in 61 is checked, in half as many cache lines as every 61st byte).
 *
 * DESIGN.md lists the small copies first; the bulk copies run first here
 * because they double as the reset of dst, which keeps run() idempotent and
 * lets one fold check both kinds of copy.  The work is the same.
 *
 * units = 16 * (1 + 2 + ... + 256) + 4 * 32768 = 526336 + 131072 bytes.
 */
#include "bench_kernel.h"
#include "bench_lib.h"

#define MC_REGION   (128u * 1024u)
#define MC_NOPS     4096u
#define MC_MAXLEN   256u
#define MC_CHUNK    (32u * 1024u)
#define MC_NCHUNKS  4u
#define MC_STRIDE   244u	/* fold the 4 bytes at every 244th offset */

#define MC_SMALL_BYTES (MC_NOPS / MC_MAXLEN * (MC_MAXLEN * (MC_MAXLEN + 1u) / 2u))
#define MC_UNITS       (MC_SMALL_BYTES + MC_NCHUNKS * MC_CHUNK)

/* One precomputed copy: dst[dst_off..+len) = src[src_off..+len). */
typedef struct mc_op {
	uint32_t src_off;
	uint32_t dst_off;
	uint32_t len;
} mc_op;

#define MC_SRC_OFF  0u
#define MC_DST_OFF  (MC_SRC_OFF + MC_REGION)
#define MC_OPS_OFF  (MC_DST_OFF + MC_REGION)
#define ARENA_BYTES (MC_OPS_OFF + BENCH_ALIGN_UP(MC_NOPS * (uint32_t)sizeof(mc_op), BENCH_LINE))

_Static_assert(MC_UNITS == 657408u, "DESIGN.md: B / 657408");

static void memcpy_init(void *arena)
{
	unsigned char *src = (unsigned char *)bench_at(arena, MC_SRC_OFF);
	mc_op *op = (mc_op *)bench_at(arena, MC_OPS_OFF);
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0x3E3C0B11u);
	for (i = 0; i < MC_REGION; i += 4) {
		uint32_t v = bench_rng_next(&rng);

		src[i + 0] = (unsigned char)v;
		src[i + 1] = (unsigned char)(v >> 8);
		src[i + 2] = (unsigned char)(v >> 16);
		src[i + 3] = (unsigned char)(v >> 24);
	}

	/* Lengths 1..MC_MAXLEN, each MC_NOPS / MC_MAXLEN times, shuffled
	 * (Fisher-Yates). */
	for (i = 0; i < MC_NOPS; i++)
		op[i].len = i % MC_MAXLEN + 1u;
	for (i = MC_NOPS - 1u; i > 0; i--) {
		uint32_t j = bench_rng_below(&rng, i + 1u);
		uint32_t t = op[i].len;

		op[i].len = op[j].len;
		op[j].len = t;
	}
	for (i = 0; i < MC_NOPS; i++) {
		op[i].src_off = bench_rng_below(&rng, MC_REGION - op[i].len + 1u);
		op[i].dst_off = bench_rng_below(&rng, MC_REGION - op[i].len + 1u);
	}
}

static double memcpy_run(void *arena)
{
	const unsigned char *src = (const unsigned char *)bench_at(arena, MC_SRC_OFF);
	unsigned char *dst = (unsigned char *)bench_at(arena, MC_DST_OFF);
	const mc_op *op = (const mc_op *)bench_at(arena, MC_OPS_OFF);
	uint32_t h = 0, i;

	for (i = 0; i < MC_NCHUNKS; i++)
		BLIB(memcpy)(dst + i * MC_CHUNK,
			     src + (i + 1u) % MC_NCHUNKS * MC_CHUNK, MC_CHUNK);

	for (i = 0; i < MC_NOPS; i++)
		BLIB(memcpy)(dst + op[i].dst_off, src + op[i].src_off, op[i].len);

	for (i = 0; i + 4u <= MC_REGION; i += MC_STRIDE) {
		const unsigned char *p = dst + i;

		h = bench_mix(h, (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
				 (uint32_t)p[2] << 8 | p[3]);
	}
	return bench_result_u32(h);
}

BENCH_KDEF(memcpy) = {
	.name = "memcpy", .desc = "4096 x 1..256 B + 4 x 32 KB", .unit = "B",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = MC_UNITS, .arena_bytes = ARENA_BYTES,
	.init = memcpy_init, .run = memcpy_run,
};
