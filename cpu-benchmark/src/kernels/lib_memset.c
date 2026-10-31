/*
 * lib_memset.c - the toolchain's newlib memset (LIB).
 *
 * One 256 KB region.  One run():
 *
 *   1. 4 aligned 64 KB fills with 0x00, 0x55, 0xAA and 0xFF (buffer clears).
 *      They cover the whole region, so they also reset it: whatever the
 *      previous run (of either variant) left there is overwritten.
 *   2. 4096 small fills, lengths 1..512 (each length exactly 8 times, in
 *      shuffled order) at arbitrary byte offsets with varying fill bytes,
 *      like clearing structs, strings and span buffers.  Offsets, lengths
 *      (u16) and values are precomputed by init.
 *   3. Fold a sample of the region: the 4 bytes at every 244th offset.
 *
 * DESIGN.md lists the small fills first; the bulk fills run first here
 * because they double as the reset, which keeps run() idempotent and lets
 * one fold check both kinds of fill.  The work is the same.
 *
 * units = 8 * (1 + 2 + ... + 512) + 4 * 65536 = 1050624 + 262144 bytes.
 */
#include "bench_kernel.h"
#include "bench_lib.h"

#define MS_REGION   (256u * 1024u)
#define MS_NOPS     4096u
#define MS_MAXLEN   512u
#define MS_CHUNK    (64u * 1024u)
#define MS_NCHUNKS  4u
#define MS_STRIDE   244u	/* fold the 4 bytes at every 244th offset */

#define MS_SMALL_BYTES (MS_NOPS / MS_MAXLEN * (MS_MAXLEN * (MS_MAXLEN + 1u) / 2u))
#define MS_UNITS       (MS_SMALL_BYTES + MS_NCHUNKS * MS_CHUNK)

/* One precomputed fill: buf[off..+len) = value. */
typedef struct ms_op {
	uint32_t off;
	uint16_t len;
	uint8_t value;
	uint8_t pad;
} ms_op;

#define MS_BUF_OFF  0u
#define MS_OPS_OFF  (MS_BUF_OFF + MS_REGION)
#define ARENA_BYTES (MS_OPS_OFF + BENCH_ALIGN_UP(MS_NOPS * (uint32_t)sizeof(ms_op), BENCH_LINE))

_Static_assert(MS_UNITS == 1312768u, "DESIGN.md: B / 1312768");
_Static_assert(sizeof(ms_op) == 8, "fill op is 8 bytes");

static void memset_init(void *arena)
{
	ms_op *op = (ms_op *)bench_at(arena, MS_OPS_OFF);
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0x3E5E7F11u);

	/* Lengths 1..MS_MAXLEN, each MS_NOPS / MS_MAXLEN times, shuffled
	 * (Fisher-Yates). */
	for (i = 0; i < MS_NOPS; i++)
		op[i].len = (uint16_t)(i % MS_MAXLEN + 1u);
	for (i = MS_NOPS - 1u; i > 0; i--) {
		uint32_t j = bench_rng_below(&rng, i + 1u);
		uint16_t t = op[i].len;

		op[i].len = op[j].len;
		op[j].len = t;
	}
	for (i = 0; i < MS_NOPS; i++) {
		op[i].off = bench_rng_below(&rng, MS_REGION - op[i].len + 1u);
		op[i].value = (uint8_t)(bench_rng_next(&rng) >> 24);
		op[i].pad = 0;
	}
}

static double memset_run(void *arena)
{
	unsigned char *buf = (unsigned char *)bench_at(arena, MS_BUF_OFF);
	const ms_op *op = (const ms_op *)bench_at(arena, MS_OPS_OFF);
	uint32_t h = 0, i;

	for (i = 0; i < MS_NCHUNKS; i++)
		BLIB(memset)(buf + i * MS_CHUNK, (int)(i * 0x55u), MS_CHUNK);

	for (i = 0; i < MS_NOPS; i++)
		BLIB(memset)(buf + op[i].off, op[i].value, op[i].len);

	for (i = 0; i + 4u <= MS_REGION; i += MS_STRIDE) {
		const unsigned char *p = buf + i;

		h = bench_mix(h, (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
				 (uint32_t)p[2] << 8 | p[3]);
	}
	return bench_result_u32(h);
}

BENCH_KDEF(memset) = {
	.name = "memset", .desc = "4096 x 1..512 B + 4 x 64 KB", .unit = "B",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = MS_UNITS, .arena_bytes = ARENA_BYTES,
	.init = memset_init, .run = memset_run,
};
