/*
 * lib_strops.c - the toolchain's newlib strlen / strcmp / strchr / memcmp (LIB).
 *
 * A string table like the ones games and tools build from config files or
 * asset names: 2048 NUL-terminated strings of 4..60 letters packed back to
 * back (so they start at every possible alignment), reached through an offset
 * table.  The strings come in neighbour pairs (2k, 2k+1): independent, sharing
 * a long prefix, identical, or one a prefix of the other.  Plus 512 pairs of
 * 64-byte blocks (aligned), a quarter of them equal and the rest differing in
 * one byte at a random position.
 *
 * run() calls, through BLIB() (each variant's own newlib build):
 *   strlen(s[i]), strcmp(s[i], s[i ^ 1]) and strchr(s[i], 'e') for every
 *   string, and memcmp(a[k], b[k], 64) for every block pair,
 * and folds the lengths, the signs of the comparisons (the magnitude is not
 * specified by C) and the strchr offsets (-1 when absent).
 * units = 2048 * 3 + 512 = 6656 calls.
 */
#include "bench_kernel.h"
#include "bench_lib.h"

#define SO_NSTR   2048u
#define SO_MINLEN 4u
#define SO_MAXLEN 60u
#define SO_NBLK   512u
#define SO_BLK    64u

#define POOL_OFF    0u
#define POOL_BYTES  (SO_NSTR * (SO_MAXLEN + 1u)) /* worst case, 124928 B */
#define OFFS_OFF    BENCH_ALIGN_UP(POOL_OFF + POOL_BYTES, BENCH_LINE)
#define BLKA_OFF    BENCH_ALIGN_UP(OFFS_OFF + SO_NSTR * 4u, BENCH_LINE)
#define BLKB_OFF    BENCH_ALIGN_UP(BLKA_OFF + SO_NBLK * SO_BLK, BENCH_LINE)
#define ARENA_BYTES BENCH_ALIGN_UP(BLKB_OFF + SO_NBLK * SO_BLK, BENCH_LINE)

static inline uint32_t sign_of(int c)
{
	return (uint32_t)((c > 0) - (c < 0));
}

static double strops_run(void *arena)
{
	const char *pool = (const char *)bench_at(arena, POOL_OFF);
	const uint32_t *off = (const uint32_t *)bench_at(arena, OFFS_OFF);
	const unsigned char *ba = (const unsigned char *)bench_at(arena, BLKA_OFF);
	const unsigned char *bb = (const unsigned char *)bench_at(arena, BLKB_OFF);
	uint32_t h = 0, i;

	for (i = 0; i < SO_NSTR; i++) {
		const char *s = pool + off[i];
		const char *t = pool + off[i ^ 1u];
		const char *e;
		size_t len = BLIB(strlen)(s);
		int c = BLIB(strcmp)(s, t);

		e = BLIB(strchr)(s, 'e');
		h = bench_mix(h, (uint32_t)len);
		h = bench_mix(h, sign_of(c));
		h = bench_mix(h, e ? (uint32_t)(e - s) : 0xFFFFFFFFu);
	}
	for (i = 0; i < SO_NBLK; i++) {
		int c = BLIB(memcmp)(ba + i * SO_BLK, bb + i * SO_BLK, SO_BLK);

		h = bench_mix(h, sign_of(c));
	}
	return bench_result_u32(h);
}

/* ---- input generation ------------------------------------------------- *
 * init is freestanding code too: stop GCC from turning its copy loops into
 * memcpy / memmove calls (the same option bench_copy and bench_fill use).
 * init is not timed. */
#define INIT_FN __attribute__((optimize("no-tree-loop-distribute-patterns")))

INIT_FN static uint8_t rand_letter(bench_rng *r)
{
	uint32_t v = bench_rng_below(r, 64);

	/* Mostly lower case, some upper case. */
	return (uint8_t)(v < 52 ? 'a' + v % 26u : 'A' + (v - 52u));
}

INIT_FN static uint32_t rand_len(bench_rng *r, uint32_t lo, uint32_t hi)
{
	return lo + bench_rng_below(r, hi - lo + 1u);
}

/* Appends len random letters to p[0..), returns len. */
INIT_FN static uint32_t put_letters(bench_rng *r, uint8_t *p, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		p[i] = rand_letter(r);
	return len;
}

INIT_FN static void strops_init(void *arena)
{
	uint8_t *pool = (uint8_t *)bench_at(arena, POOL_OFF);
	uint32_t *off = (uint32_t *)bench_at(arena, OFFS_OFF);
	uint8_t *ba = (uint8_t *)bench_at(arena, BLKA_OFF);
	uint8_t *bb = (uint8_t *)bench_at(arena, BLKB_OFF);
	bench_rng rng;
	uint32_t pos = 0, k, i;

	bench_rng_seed(&rng, 0x57A1B0B5u);
	for (k = 0; k < SO_NSTR; k += 2) {
		uint32_t kind = bench_rng_below(&rng, 8);
		uint32_t a = pos, la, lb, n;

		/* First string of the pair. */
		la = rand_len(&rng, kind >= 3 ? 24u : SO_MINLEN, SO_MAXLEN);
		pos += put_letters(&rng, pool + pos, la);
		pool[pos++] = 0;
		off[k] = a;
		off[k + 1] = pos;

		/* Second string: built from the first one (read back from the
		 * pool, which never overlaps the bytes being written). */
		if (kind < 3) {                      /* independent */
			lb = rand_len(&rng, SO_MINLEN, SO_MAXLEN);
			pos += put_letters(&rng, pool + pos, lb);
		} else if (kind < 6) {               /* long common prefix */
			n = rand_len(&rng, la / 2u, la - 1u);
			for (i = 0; i < n; i++)
				pool[pos + i] = pool[a + i];
			do
				pool[pos + n] = rand_letter(&rng);
			while (pool[pos + n] == pool[a + n]);
			lb = rand_len(&rng, n + 1u, SO_MAXLEN);
			put_letters(&rng, pool + pos + n + 1u, lb - n - 1u);
			pos += lb;
		} else if (kind == 6) {              /* identical */
			for (i = 0; i < la; i++)
				pool[pos + i] = pool[a + i];
			pos += la;
		} else {                             /* proper prefix */
			lb = rand_len(&rng, SO_MINLEN, la - 1u);
			for (i = 0; i < lb; i++)
				pool[pos + i] = pool[a + i];
			pos += lb;
		}
		pool[pos++] = 0;
	}

	for (k = 0; k < SO_NBLK; k++) {
		uint8_t *pa = ba + k * SO_BLK, *pb = bb + k * SO_BLK;

		for (i = 0; i < SO_BLK; i++) {
			pa[i] = (uint8_t)bench_rng_next(&rng);
			pb[i] = pa[i];
		}
		if ((k & 3u) != 0) {
			i = bench_rng_below(&rng, SO_BLK);
			pb[i] = (uint8_t)(pa[i] ^ (1u + bench_rng_below(&rng, 255)));
		}
	}
}

BENCH_KDEF(strops) = {
	.name = "strops", .desc = "strlen/strcmp/strchr/memcmp", .unit = "call",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = SO_NSTR * 3u + SO_NBLK, .arena_bytes = ARENA_BYTES,
	.init = strops_init, .run = strops_run,
};
