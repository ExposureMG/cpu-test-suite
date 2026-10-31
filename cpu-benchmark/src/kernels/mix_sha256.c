/*
 * mix_sha256.c - MIX: SHA-256 (FIPS 180-4) compression over 16 KB.
 *
 * The block function every crypto library, package manager and console
 * signature checker carries: message words are read big-endian from the
 * byte stream, the 48-word message schedule is expanded with the small
 * sigma functions, and 64 rounds update the eight working variables.  The
 * rounds are unrolled 8 at a time with the working variables renamed instead
 * of shifted (the usual embedded-library form), so the loop body is pure
 * 32-bit rotate / xor / and / add work: constant rotates (rotlwi), no
 * multiplies, no variable shifts, and a 256-byte schedule array on the
 * stack.  It measures integer ALU throughput and how well each compiler
 * schedules long dependent chains on a dual-issue in-order core.
 *
 * Workload: one message of 256 random 64-byte blocks, hashed exactly (the
 * chaining value runs through all 256 blocks; there is no padding block, so
 * the result is the state after the last compression, not a standard digest
 * of the 16 KB).  The bytes are generated one at a time, so the message (and
 * the result) are the same on either host byte order.
 *
 * Result: bench_mix fold of the 8 state words.  EXACT.
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   OFF_DATA  uint8_t msg[256][64]   16 KB  (read-only in run)
 */
#include "bench_kernel.h"

#define SHA_BLOCK_BYTES 64u
#define SHA_BLOCKS      256u
#define SHA_MSG_BYTES   (SHA_BLOCKS * SHA_BLOCK_BYTES) /* 16384 */

#define OFF_DATA    0u
#define ARENA_BYTES (OFF_DATA + SHA_MSG_BYTES)

/* FIPS 180-4 4.2.2: first 32 bits of the fractional parts of the cube roots
 * of the first 64 primes. */
static const uint32_t sha256_k[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* FIPS 180-4 5.3.3: initial hash value H(0). */
static const uint32_t sha256_h0[8] = {
	0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
	0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* FIPS 180-4 4.1.2 */
#define CH(x, y, z)  ((z) ^ ((x) & ((y) ^ (z))))
#define MAJ(x, y, z) (((x) & (y)) | ((z) & ((x) | (y))))
#define BSIG0(x)     (ROTR32(x, 2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define BSIG1(x)     (ROTR32(x, 6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SSIG0(x)     (ROTR32(x, 7) ^ ROTR32(x, 18) ^ ((x) >> 3))
#define SSIG1(x)     (ROTR32(x, 17) ^ ROTR32(x, 19) ^ ((x) >> 10))

/* One round with the working variables renamed: the new 'a' lands in h and
 * the new 'e' in d, so the next round is called with the names rotated. */
#define SHA_ROUND(a, b, c, d, e, f, g, h, i)                               \
	do {                                                               \
		uint32_t t1_ = (h) + BSIG1(e) + CH(e, f, g) + sha256_k[i] + \
			       w[i];                                        \
		uint32_t t2_ = BSIG0(a) + MAJ(a, b, c);                    \
		(d) += t1_;                                                \
		(h) = t1_ + t2_;                                           \
	} while (0)

static inline uint32_t load_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* Compress one 64-byte block into the chaining state st[8]. */
static void sha256_block(uint32_t st[8], const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned i;

	for (i = 0; i < 16; i++)
		w[i] = load_be32(p + 4 * i);
	for (i = 16; i < 64; i++)
		w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

	a = st[0];
	b = st[1];
	c = st[2];
	d = st[3];
	e = st[4];
	f = st[5];
	g = st[6];
	h = st[7];
	for (i = 0; i < 64; i += 8) {
		SHA_ROUND(a, b, c, d, e, f, g, h, i + 0);
		SHA_ROUND(h, a, b, c, d, e, f, g, i + 1);
		SHA_ROUND(g, h, a, b, c, d, e, f, i + 2);
		SHA_ROUND(f, g, h, a, b, c, d, e, i + 3);
		SHA_ROUND(e, f, g, h, a, b, c, d, i + 4);
		SHA_ROUND(d, e, f, g, h, a, b, c, i + 5);
		SHA_ROUND(c, d, e, f, g, h, a, b, i + 6);
		SHA_ROUND(b, c, d, e, f, g, h, a, i + 7);
	}
	st[0] += a;
	st[1] += b;
	st[2] += c;
	st[3] += d;
	st[4] += e;
	st[5] += f;
	st[6] += g;
	st[7] += h;
}

static void sha256_init(void *arena)
{
	uint8_t *msg = bench_at(arena, OFF_DATA);
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0x5AA25601u);
	for (i = 0; i < SHA_MSG_BYTES; i += 4) {
		uint32_t x = bench_rng_next(&r);

		msg[i + 0] = (uint8_t)x;
		msg[i + 1] = (uint8_t)(x >> 8);
		msg[i + 2] = (uint8_t)(x >> 16);
		msg[i + 3] = (uint8_t)(x >> 24);
	}
}

static double sha256_run(void *arena)
{
	const uint8_t *msg = bench_at(arena, OFF_DATA);
	uint32_t st[8], hash = 0;
	uint32_t i;

	for (i = 0; i < 8; i++)
		st[i] = sha256_h0[i];
	for (i = 0; i < SHA_BLOCKS; i++)
		sha256_block(st, msg + i * SHA_BLOCK_BYTES);
	for (i = 0; i < 8; i++)
		hash = bench_mix(hash, st[i]);
	return bench_result_u32(hash);
}

BENCH_KDEF(sha256) = {
	.name = "sha256", .desc = "SHA-256 compress, 256 blk 16 KB", .unit = "B",
	.category = BENCH_CAT_MIX, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = SHA_MSG_BYTES, .arena_bytes = ARENA_BYTES,
	.init = sha256_init, .run = sha256_run,
};
