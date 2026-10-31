/*
 * mem_bswap.c - little-endian file records to native structs (MEM).
 *
 * A game ported from the PC loads its data files unchanged: 8192 records of
 * 32 bytes, stored little-endian,
 *
 *     { u32 id; u16 a, b; u64 ts; f32 x, y; u32 flags; u16 c, d; }
 *
 * run() decodes every record into the native struct (big-endian on Xenon)
 * with aligned u16/u32/u64 loads and __builtin_bswap16/32/64.  The float
 * fields are swapped as their 32-bit patterns and stored through a union:
 * no FP arithmetic, so the bits are preserved.  A few fields per record are
 * folded on the fly (adds and xors, cheap next to the ~10 stores a record
 * costs), and every 61st decoded record is read back from the output so the
 * stores are checked as well.  256 KB in, 256 KB out.
 *
 * init writes the file image byte by byte, so it is little-endian on every
 * host.  On the little-endian host build the same swaps produce
 * byte-reversed values; nothing branches on or indexes with decoded data,
 * so both byte orders run the same code path (only the checksum differs).
 */
#include "bench_kernel.h"

#define BSW_RECS      8192u
#define BSW_REC_BYTES 32u
#define BSW_IN_OFF    0u
#define BSW_OUT_OFF   (BSW_IN_OFF + BSW_RECS * BSW_REC_BYTES)
#define ARENA_BYTES   (BSW_OUT_OFF + BSW_RECS * BSW_REC_BYTES)
#define BSW_SAMPLE    61u	/* read back every 61st decoded record */

/* The record as stored in the file: little-endian, floats as bit patterns. */
typedef struct bsw_disk {
	uint32_t id;
	uint16_t a, b;
	uint64_t ts;
	uint32_t x, y;
	uint32_t flags;
	uint16_t c, d;
} bsw_disk;

/* The in-memory record the game works with. */
typedef struct bsw_rec {
	uint32_t id;
	uint16_t a, b;
	uint64_t ts;
	float x, y;
	uint32_t flags;
	uint16_t c, d;
} bsw_rec;

typedef union bsw_f32 {
	uint32_t u;
	float f;
} bsw_f32;

_Static_assert(sizeof(bsw_disk) == BSW_REC_BYTES, "file record is 32 bytes");
_Static_assert(sizeof(bsw_rec) == BSW_REC_BYTES, "native record is 32 bytes");

/* Store the low 'bytes' bytes of v at p, least significant first. */
static void bsw_put_le(unsigned char *p, uint64_t v, uint32_t bytes)
{
	uint32_t i;

	for (i = 0; i < bytes; i++) {
		p[i] = (unsigned char)v;
		v >>= 8;
	}
}

static void bswap_init(void *arena)
{
	unsigned char *p = (unsigned char *)bench_at(arena, BSW_IN_OFF);
	/* A Windows FILETIME around 2024 (100 ns ticks since 1601). */
	uint64_t ts = 0x01DA3C5E12345678ull;
	bench_rng rng;
	uint32_t i;

	bench_rng_seed(&rng, 0xB5A9E001u);
	for (i = 0; i < BSW_RECS; i++, p += BSW_REC_BYTES) {
		uint32_t r0 = bench_rng_next(&rng);
		uint32_t r1 = bench_rng_next(&rng);
		uint32_t r2 = bench_rng_next(&rng);
		uint32_t r3 = bench_rng_next(&rng);
		bsw_f32 x, y;

		/* World coordinates in [-4096, 4096), 1/256 unit steps: exact. */
		x.f = (float)((int32_t)(r0 >> 11) - 1048576) * (1.0f / 256.0f);
		y.f = (float)((int32_t)(r1 >> 11) - 1048576) * (1.0f / 256.0f);
		ts += 166667u + (r2 & 0xFFFu);	/* ~one 60 Hz frame apart */

		bsw_put_le(p + 0, 0x10000u + i * 5u + (r3 & 3u), 4);	/* id */
		bsw_put_le(p + 4, r2 >> 16, 2);				/* a */
		bsw_put_le(p + 6, r3 >> 16, 2);				/* b */
		bsw_put_le(p + 8, ts, 8);				/* ts */
		bsw_put_le(p + 16, x.u, 4);				/* x */
		bsw_put_le(p + 20, y.u, 4);				/* y */
		bsw_put_le(p + 24, r0 ^ r3, 4);				/* flags */
		bsw_put_le(p + 28, r1 & 0xFFFFu, 2);			/* c */
		bsw_put_le(p + 30, (r2 ^ r1) & 0xFFFFu, 2);		/* d */
	}
}

/* Decode n file records into out[], return a checksum of a few fields. */
static uint32_t bsw_decode(bsw_rec *restrict out, const bsw_disk *restrict in,
			   uint32_t n)
{
	uint32_t ids = 0, flg = 0, h16 = 0, fxy = 0, i, h;
	uint64_t tsx = 0;

	for (i = 0; i < n; i++) {
		const bsw_disk *s = &in[i];
		bsw_rec *d = &out[i];
		bsw_f32 x, y;

		x.u = __builtin_bswap32(s->x);
		y.u = __builtin_bswap32(s->y);

		d->id = __builtin_bswap32(s->id);
		d->a = __builtin_bswap16(s->a);
		d->b = __builtin_bswap16(s->b);
		d->ts = __builtin_bswap64(s->ts);
		d->x = x.f;
		d->y = y.f;
		d->flags = __builtin_bswap32(s->flags);
		d->c = __builtin_bswap16(s->c);
		d->d = __builtin_bswap16(s->d);

		ids += d->id;
		flg ^= d->flags;
		tsx ^= d->ts;
		h16 += (uint32_t)d->a + d->d;
		fxy += x.u ^ y.u;
	}

	h = bench_mix(0, ids);
	h = bench_mix(h, flg);
	h = bench_mix(h, (uint32_t)tsx);
	h = bench_mix(h, (uint32_t)(tsx >> 32));
	h = bench_mix(h, h16);
	return bench_mix(h, fxy);
}

static inline uint32_t bsw_fold(uint32_t h, uint32_t v)
{
	return ((h << 5) | (h >> 27)) ^ v;
}

static double bswap_run(void *arena)
{
	const bsw_disk *in = (const bsw_disk *)bench_at(arena, BSW_IN_OFF);
	bsw_rec *out = (bsw_rec *)bench_at(arena, BSW_OUT_OFF);
	uint32_t h, i;

	h = bsw_decode(out, in, BSW_RECS);

	/* Read a sample of the decoded records back from memory. */
	for (i = 0; i < BSW_RECS; i += BSW_SAMPLE) {
		const bsw_rec *d = &out[i];
		bsw_f32 x, y;

		x.f = d->x;
		y.f = d->y;
		h = bsw_fold(h, d->id);
		h = bsw_fold(h, ((uint32_t)d->a << 16) | d->b);
		h = bsw_fold(h, (uint32_t)(d->ts >> 32));
		h = bsw_fold(h, (uint32_t)d->ts);
		h = bsw_fold(h, x.u);
		h = bsw_fold(h, y.u);
		h = bsw_fold(h, d->flags);
		h = bsw_fold(h, ((uint32_t)d->c << 16) | d->d);
	}
	return bench_result_u32(bench_mix(h, BSW_RECS));
}

BENCH_KDEF(bswap) = {
	.name = "bswap", .desc = "8192 LE 32 B records -> native", .unit = "B",
	.category = BENCH_CAT_MEM, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = BSW_RECS * BSW_REC_BYTES, .arena_bytes = ARENA_BYTES,
	.init = bswap_init, .run = bswap_run,
};
