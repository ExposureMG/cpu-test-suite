/*
 * int_crc32.c - zlib CRC-32 (reflected polynomial 0xEDB88320) with
 * slicing-by-4 tables, over a 64 KB buffer of random bytes.
 *
 * init builds the four 256-entry tables (4 KB) and the buffer in the arena.
 * run is zlib's crc32_little() loop: XOR four message bytes into the CRC,
 * then four table lookups replace 32 single-bit steps.  The buffer is a byte
 * stream, so each 32-bit chunk is assembled from bytes in little-endian order
 * (the order the reflected CRC consumes them); the CRC is the same on any
 * host byte order.  Initial value and final XOR are 0xFFFFFFFF, so the result
 * equals zlib's crc32(0, buf, 65536).
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   CRC_TAB_OFF  uint32_t tab[4][256]   4 KB  (read-only in run)
 *   CRC_BUF_OFF  unsigned char [65536] 64 KB  (read-only in run)
 */
#include "bench_kernel.h"

#define CRC_POLY      0xEDB88320u
#define CRC_TAB_OFF   0u
#define CRC_BUF_OFF   4096u
#define CRC_BUF_BYTES 65536u
#define ARENA_BYTES   (CRC_BUF_OFF + CRC_BUF_BYTES)

typedef uint32_t crc_table[4][256];

static void crc32_init(void *arena)
{
	crc_table *tab = bench_at(arena, CRC_TAB_OFF);
	unsigned char *buf = bench_at(arena, CRC_BUF_OFF);
	bench_rng r;
	uint32_t i, k, c;

	/* Byte-at-a-time table: the CRC of each byte value. */
	for (i = 0; i < 256; i++) {
		c = i;
		for (k = 0; k < 8; k++)
			c = (c & 1u) ? (c >> 1) ^ CRC_POLY : c >> 1;
		(*tab)[0][i] = c;
	}
	/* tab[k][n]: the CRC of byte n followed by k zero bytes. */
	for (i = 0; i < 256; i++) {
		c = (*tab)[0][i];
		for (k = 1; k < 4; k++) {
			c = (*tab)[0][c & 0xFFu] ^ (c >> 8);
			(*tab)[k][i] = c;
		}
	}

	/* Message bytes, written one at a time so the stream is the same on
	 * either byte order. */
	bench_rng_seed(&r, 0xC5C32001u);
	for (i = 0; i < CRC_BUF_BYTES; i += 4) {
		uint32_t x = bench_rng_next(&r);

		buf[i + 0] = (unsigned char)x;
		buf[i + 1] = (unsigned char)(x >> 8);
		buf[i + 2] = (unsigned char)(x >> 16);
		buf[i + 3] = (unsigned char)(x >> 24);
	}
}

/* crc is the running (pre-inverted) CRC register.  Words are read at
 * 4-byte steps from p; a trailing partial word is done bytewise. */
static uint32_t crc32_slice4(const crc_table *tab, uint32_t crc,
			     const unsigned char *p, uint32_t len)
{
	const uint32_t *t0 = (*tab)[0], *t1 = (*tab)[1];
	const uint32_t *t2 = (*tab)[2], *t3 = (*tab)[3];

	while (len >= 4) {
		crc ^= (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
		crc = t3[crc & 0xFFu] ^ t2[(crc >> 8) & 0xFFu] ^
		      t1[(crc >> 16) & 0xFFu] ^ t0[crc >> 24];
		p += 4;
		len -= 4;
	}
	while (len--)
		crc = t0[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
	return crc;
}

static double crc32_run(void *arena)
{
	const crc_table *tab = bench_at(arena, CRC_TAB_OFF);
	const unsigned char *buf = bench_at(arena, CRC_BUF_OFF);
	uint32_t crc;

	crc = crc32_slice4(tab, 0xFFFFFFFFu, buf, CRC_BUF_BYTES);
	return bench_result_u32(crc ^ 0xFFFFFFFFu);
}

BENCH_KDEF(crc32) = {
	.name = "crc32", .desc = "CRC-32 slice-by-4, 64 KB", .unit = "B",
	.category = BENCH_CAT_INT, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = CRC_BUF_BYTES, .arena_bytes = ARENA_BYTES,
	.init = crc32_init, .run = crc32_run,
};
