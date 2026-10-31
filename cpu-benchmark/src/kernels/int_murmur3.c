/*
 * int_murmur3.c - MurmurHash3_x86_32 over 4096 keys, the way a hash table or
 * string-interning pass hashes its keys.
 *
 * The keys live back to back in a byte pool, each starting at a 4-byte
 * aligned offset.  Key lengths cycle through 1..64 (every length exactly 64
 * times, shuffled), so one run hashes exactly 64 * (1 + 2 + ... + 64) =
 * 133120 bytes.  The body reads 4-byte blocks assembled from bytes in
 * little-endian order, like the reference implementation on x86 (so hashes
 * match the published MurmurHash3 values on either byte order); the tail
 * is the reference switch with fallthrough, then fmix32.
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   MM_KEY_OFF   mm_key[4096]   32 KB  (offset into the pool, length)
 *   MM_POOL_OFF  key bytes     136 KB  (sum of the lengths rounded up to 4)
 */
#include "bench_kernel.h"

#define MM_KEYS      4096u
#define MM_MAXLEN    64u                  /* multiple of 4 */
#define MM_CYCLES    (MM_KEYS / MM_MAXLEN) /* times each length occurs */
#define MM_Q         (MM_MAXLEN / 4u)
/* Bytes hashed per run: every cycle is 1 + 2 + ... + MM_MAXLEN bytes. */
#define MM_BYTES     (MM_CYCLES * (MM_MAXLEN * (MM_MAXLEN + 1u) / 2u))
/* Pool bytes: lengths 4q-3..4q all round up to 4q, so one cycle takes
 * 16 * (1 + 2 + ... + MM_MAXLEN / 4) bytes. */
#define MM_POOL      (MM_CYCLES * 16u * (MM_Q * (MM_Q + 1u) / 2u))
#define MM_SEED      0x9747B28Cu

#define MM_KEY_OFF   0u
#define MM_POOL_OFF  (MM_KEY_OFF + MM_KEYS * 8u)
#define ARENA_BYTES  (MM_POOL_OFF + MM_POOL)

_Static_assert(MM_BYTES == 133120u, "DESIGN.md: murmur3 hashes 133120 B");
_Static_assert(MM_POOL % BENCH_LINE == 0u, "pool size");

typedef struct mm_key {
	uint32_t off; /* byte offset into the pool, multiple of 4 */
	uint32_t len; /* 1..64 */
} mm_key;

static inline uint32_t mm_rotl32(uint32_t x, uint32_t r)
{
	return (x << r) | (x >> (32u - r));
}

static inline uint32_t mm_fmix32(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

static uint32_t murmur3_32(const unsigned char *key, uint32_t len, uint32_t seed)
{
	const uint32_t c1 = 0xCC9E2D51u;
	const uint32_t c2 = 0x1B873593u;
	const uint32_t nblocks = len / 4u;
	const unsigned char *tail = key + nblocks * 4u;
	uint32_t h1 = seed;
	uint32_t k1, i;

	for (i = 0; i < nblocks; i++) {
		const unsigned char *b = key + i * 4u;

		k1 = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
		     (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
		k1 *= c1;
		k1 = mm_rotl32(k1, 15);
		k1 *= c2;

		h1 ^= k1;
		h1 = mm_rotl32(h1, 13);
		h1 = h1 * 5u + 0xE6546B64u;
	}

	k1 = 0;
	switch (len & 3u) {
	case 3:
		k1 ^= (uint32_t)tail[2] << 16;
		__attribute__((fallthrough));
	case 2:
		k1 ^= (uint32_t)tail[1] << 8;
		__attribute__((fallthrough));
	case 1:
		k1 ^= tail[0];
		k1 *= c1;
		k1 = mm_rotl32(k1, 15);
		k1 *= c2;
		h1 ^= k1;
		break;
	default:
		break;
	}

	h1 ^= len;
	return mm_fmix32(h1);
}

static void murmur3_init(void *arena)
{
	mm_key *keys = bench_at(arena, MM_KEY_OFF);
	unsigned char *pool = bench_at(arena, MM_POOL_OFF);
	bench_rng r;
	uint32_t i, off;

	bench_rng_seed(&r, 0x3A3A0001u);

	/* Lengths 1..64, each MM_KEYS / MM_MAXLEN times, Fisher-Yates shuffled. */
	for (i = 0; i < MM_KEYS; i++)
		keys[i].len = i % MM_MAXLEN + 1u;
	for (i = MM_KEYS - 1u; i > 0; i--) {
		uint32_t j = bench_rng_below(&r, i + 1u);
		uint32_t t = keys[i].len;

		keys[i].len = keys[j].len;
		keys[j].len = t;
	}

	/* Pack the keys in order, each padded to a 4-byte boundary. */
	off = 0;
	for (i = 0; i < MM_KEYS; i++) {
		keys[i].off = off;
		off += BENCH_ALIGN_UP(keys[i].len, 4u);
	}

	/* Random key bytes (the padding bytes too), written one at a time. */
	for (i = 0; i < MM_POOL; i += 4) {
		uint32_t x = bench_rng_next(&r);

		pool[i + 0] = (unsigned char)x;
		pool[i + 1] = (unsigned char)(x >> 8);
		pool[i + 2] = (unsigned char)(x >> 16);
		pool[i + 3] = (unsigned char)(x >> 24);
	}
}

static double murmur3_run(void *arena)
{
	const mm_key *keys = bench_at(arena, MM_KEY_OFF);
	const unsigned char *pool = bench_at(arena, MM_POOL_OFF);
	uint32_t h = 0, i;

	for (i = 0; i < MM_KEYS; i++)
		h = bench_mix(h, murmur3_32(pool + keys[i].off, keys[i].len, MM_SEED));
	return bench_result_u32(h);
}

BENCH_KDEF(murmur3) = {
	.name = "murmur3", .desc = "MurmurHash3_x86_32, 4096 keys", .unit = "B",
	.category = BENCH_CAT_INT, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = MM_BYTES, .arena_bytes = ARENA_BYTES,
	.init = murmur3_init, .run = murmur3_run,
};
