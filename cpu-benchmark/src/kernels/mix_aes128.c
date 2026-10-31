/*
 * mix_aes128.c - MIX: AES-128 in CTR mode with 4 T-tables, 16 KB.
 *
 * The classic 32-bit software AES (rijndael-alg-fst / OpenSSL aes_core
 * style) that save-game crypto, network stacks and homebrew loaders use:
 * each round is 16 table lookups (Te0..Te3, 1 KB each) indexed by the bytes
 * of the four state columns, xored together with the round key.  The last
 * round has no MixColumns; it takes the S-box byte back out of the tables by
 * masking (Te2 & 0xff000000, Te3 & 0x00ff0000, ...), so the kernel needs no
 * fifth table.  Byte extraction plus index scaling is one rlwinm per lookup,
 * so a round is load / xor bound: it measures L1 load throughput and the
 * scheduling of 16 independent loads per round on an in-order core.
 *
 * init builds the tables from the static S-box, expands a random 128-bit key
 * (FIPS-197 5.2) and generates the 16 KB plaintext and the initial counter
 * block.  run encrypts 1024 counter blocks (a 128-bit big-endian counter
 * whose low word starts at 0xfffffe00 + x, x < 256, so it wraps and carries
 * into the next word once per run, here at block 448), xors the keystream
 * into the plaintext and writes the ciphertext to the arena.  Plaintext,
 * counter and ciphertext are byte streams (assembled / stored big-endian, as
 * FIPS-197 orders the state bytes), so the result is the same on either
 * host byte order.  Validated against FIPS-197 (appendices B and C.1, key
 * schedule A.1), SP 800-38A F.5.1 (CTR) and OpenSSL's AES-128-CTR over the
 * kernel's own 16 KB.
 *
 * Result: bench_mix fold, per block, of the xor of the 4 ciphertext words.
 * EXACT.
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   OFF_TE   uint32_t te[4][256]   4 KB   (read-only in run)
 *   OFF_RK   uint32_t rk[44]       176 B  (read-only in run)
 *   OFF_CTR  uint8_t ctr[16]       16 B   (initial counter, read-only)
 *   OFF_IN   uint8_t pt[16384]     16 KB  (read-only in run)
 *   OFF_OUT  uint8_t ct[16384]     16 KB  (written by run)
 */
#include "bench_kernel.h"

#define AES_BLOCKS 1024u
#define AES_BYTES  (AES_BLOCKS * 16u) /* 16384 */
#define AES_ROUNDS 10

#define OFF_TE      0u
#define OFF_RK      4096u
#define OFF_CTR     (OFF_RK + 256u)
#define OFF_IN      (OFF_CTR + BENCH_LINE)
#define OFF_OUT     (OFF_IN + AES_BYTES)
#define ARENA_BYTES (OFF_OUT + AES_BYTES)

typedef uint32_t aes_tables[4][256];

/* FIPS-197 figure 7. */
static const uint8_t aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
	0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
	0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
	0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
	0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
	0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
	0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
	0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
	0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
	0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
	0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
	0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/* Round constants x^(i-1) in GF(2^8), in the top byte (FIPS-197 5.2). */
static const uint32_t aes_rcon[10] = {
	0x01000000u, 0x02000000u, 0x04000000u, 0x08000000u, 0x10000000u,
	0x20000000u, 0x40000000u, 0x80000000u, 0x1b000000u, 0x36000000u,
};

static inline uint32_t get_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Te0[x] = S[x].{02,01,01,03} (one MixColumns column), Te1..Te3 are Te0
 * rotated right by 8, 16 and 24 bits. */
static void aes_build_tables(aes_tables *te)
{
	uint32_t i;

	for (i = 0; i < 256; i++) {
		uint32_t s = aes_sbox[i];
		uint32_t s2 = ((s << 1) ^ ((s & 0x80u) ? 0x1bu : 0u)) & 0xffu;
		uint32_t s3 = s2 ^ s;
		uint32_t t = s2 << 24 | s << 16 | s << 8 | s3;

		(*te)[0][i] = t;
		(*te)[1][i] = t >> 8 | t << 24;
		(*te)[2][i] = t >> 16 | t << 16;
		(*te)[3][i] = t >> 24 | t << 8;
	}
}

/* FIPS-197 5.2 key expansion for Nk = 4: 44 words. */
static void aes128_expand_key(uint32_t rk[44], const uint8_t key[16])
{
	uint32_t i;

	for (i = 0; i < 4; i++)
		rk[i] = get_be32(key + 4 * i);
	for (i = 4; i < 44; i++) {
		uint32_t t = rk[i - 1];

		if ((i & 3u) == 0) {
			/* RotWord, SubWord, Rcon */
			t = (uint32_t)aes_sbox[(t >> 16) & 0xffu] << 24 |
			    (uint32_t)aes_sbox[(t >> 8) & 0xffu] << 16 |
			    (uint32_t)aes_sbox[t & 0xffu] << 8 |
			    (uint32_t)aes_sbox[t >> 24];
			t ^= aes_rcon[i / 4 - 1];
		}
		rk[i] = rk[i - 4] ^ t;
	}
}

/* CTR mode over nblocks 16-byte blocks: out = in ^ E(K, ctr++), with the
 * counter a 128-bit big-endian integer.  Returns the fold of the
 * ciphertext. */
static uint32_t aes128_ctr(const aes_tables *te, const uint32_t *rk,
			   const uint8_t *ctr0, const uint8_t *in, uint8_t *out,
			   uint32_t nblocks)
{
	const uint32_t *t0 = (*te)[0], *t1 = (*te)[1];
	const uint32_t *t2 = (*te)[2], *t3 = (*te)[3];
	uint32_t c0 = get_be32(ctr0), c1 = get_be32(ctr0 + 4);
	uint32_t c2 = get_be32(ctr0 + 8), c3 = get_be32(ctr0 + 12);
	uint32_t hash = 0, blk;

	for (blk = 0; blk < nblocks; blk++) {
		const uint32_t *k = rk;
		uint32_t s0 = c0 ^ k[0], s1 = c1 ^ k[1];
		uint32_t s2 = c2 ^ k[2], s3 = c3 ^ k[3];
		uint32_t u0, u1, u2, u3;
		int r;

		/* Rounds 1..9: SubBytes, ShiftRows, MixColumns, AddRoundKey. */
		for (r = 1; r < AES_ROUNDS; r++) {
			k += 4;
			u0 = t0[s0 >> 24] ^ t1[(s1 >> 16) & 0xffu] ^
			     t2[(s2 >> 8) & 0xffu] ^ t3[s3 & 0xffu] ^ k[0];
			u1 = t0[s1 >> 24] ^ t1[(s2 >> 16) & 0xffu] ^
			     t2[(s3 >> 8) & 0xffu] ^ t3[s0 & 0xffu] ^ k[1];
			u2 = t0[s2 >> 24] ^ t1[(s3 >> 16) & 0xffu] ^
			     t2[(s0 >> 8) & 0xffu] ^ t3[s1 & 0xffu] ^ k[2];
			u3 = t0[s3 >> 24] ^ t1[(s0 >> 16) & 0xffu] ^
			     t2[(s1 >> 8) & 0xffu] ^ t3[s2 & 0xffu] ^ k[3];
			s0 = u0;
			s1 = u1;
			s2 = u2;
			s3 = u3;
		}

		/* Round 10: no MixColumns; mask the S-box byte out of the tables. */
		k += 4;
		u0 = (t2[s0 >> 24] & 0xff000000u) ^ (t3[(s1 >> 16) & 0xffu] & 0x00ff0000u) ^
		     (t0[(s2 >> 8) & 0xffu] & 0x0000ff00u) ^ (t1[s3 & 0xffu] & 0x000000ffu) ^ k[0];
		u1 = (t2[s1 >> 24] & 0xff000000u) ^ (t3[(s2 >> 16) & 0xffu] & 0x00ff0000u) ^
		     (t0[(s3 >> 8) & 0xffu] & 0x0000ff00u) ^ (t1[s0 & 0xffu] & 0x000000ffu) ^ k[1];
		u2 = (t2[s2 >> 24] & 0xff000000u) ^ (t3[(s3 >> 16) & 0xffu] & 0x00ff0000u) ^
		     (t0[(s0 >> 8) & 0xffu] & 0x0000ff00u) ^ (t1[s1 & 0xffu] & 0x000000ffu) ^ k[2];
		u3 = (t2[s3 >> 24] & 0xff000000u) ^ (t3[(s0 >> 16) & 0xffu] & 0x00ff0000u) ^
		     (t0[(s1 >> 8) & 0xffu] & 0x0000ff00u) ^ (t1[s2 & 0xffu] & 0x000000ffu) ^ k[3];

		/* Keystream xor plaintext. */
		u0 ^= get_be32(in);
		u1 ^= get_be32(in + 4);
		u2 ^= get_be32(in + 8);
		u3 ^= get_be32(in + 12);
		put_be32(out, u0);
		put_be32(out + 4, u1);
		put_be32(out + 8, u2);
		put_be32(out + 12, u3);
		hash = bench_mix(hash, u0 ^ u1 ^ u2 ^ u3);

		/* 128-bit big-endian increment. */
		if (++c3 == 0 && ++c2 == 0 && ++c1 == 0)
			++c0;
		in += 16;
		out += 16;
	}
	return hash;
}

static void aes128_init(void *arena)
{
	aes_tables *te = bench_at(arena, OFF_TE);
	uint32_t *rk = bench_at(arena, OFF_RK);
	uint8_t *ctr = bench_at(arena, OFF_CTR);
	uint8_t *pt = bench_at(arena, OFF_IN);
	uint8_t key[16];
	bench_rng r;
	uint32_t i, x;

	aes_build_tables(te);

	bench_rng_seed(&r, 0xAE5128C7u);
	for (i = 0; i < 16; i += 4) {
		x = bench_rng_next(&r);
		put_be32(key + i, x);
	}
	aes128_expand_key(rk, key);

	/* Counter block: 96 random bits, then a low word 0xfffffexx so the
	 * low word wraps (and carries into the next one) partway through. */
	for (i = 0; i < 12; i += 4)
		put_be32(ctr + i, bench_rng_next(&r));
	put_be32(ctr + 12, 0xfffffe00u | (bench_rng_next(&r) & 0xffu));

	for (i = 0; i < AES_BYTES; i += 4) {
		x = bench_rng_next(&r);
		pt[i + 0] = (uint8_t)x;
		pt[i + 1] = (uint8_t)(x >> 8);
		pt[i + 2] = (uint8_t)(x >> 16);
		pt[i + 3] = (uint8_t)(x >> 24);
	}
}

static double aes128_run(void *arena)
{
	const aes_tables *te = bench_at(arena, OFF_TE);
	const uint32_t *rk = bench_at(arena, OFF_RK);
	const uint8_t *ctr = bench_at(arena, OFF_CTR);
	const uint8_t *pt = bench_at(arena, OFF_IN);
	uint8_t *ct = bench_at(arena, OFF_OUT);

	return bench_result_u32(aes128_ctr(te, rk, ctr, pt, ct, AES_BLOCKS));
}

BENCH_KDEF(aes128) = {
	.name = "aes128", .desc = "AES-128 CTR, 4 T-tables, 16 KB", .unit = "B",
	.category = BENCH_CAT_MIX, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = AES_BYTES, .arena_bytes = ARENA_BYTES,
	.init = aes128_init, .run = aes128_run,
};
