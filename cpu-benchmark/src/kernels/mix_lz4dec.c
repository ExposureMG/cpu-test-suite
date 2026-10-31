/*
 * mix_lz4dec.c - MIX: LZ4 block-format decoder over 128 KB of text.
 *
 * The decompressor games and emulators use for save states, asset packs
 * and texture streaming.  Each sequence is a token byte (literal length in
 * the high nibble, match length - 4 in the low nibble, 15 meaning "more
 * length bytes follow, 255 meaning keep going"), the literals, a 16-bit
 * little-endian offset (assembled from two bytes) and the match copy.  The
 * decoder is the safe kind that checks every length against both buffers:
 * it never reads past the compressed size, never writes past the 128 KB
 * output and never lets a match reach before the start of the output.
 * Matches with an offset below 8 overlap the bytes being written (an
 * offset of 1 is a run), so they are copied byte by byte; everything else
 * (literals, distant matches) moves in 8-byte chunks of explicit byte loads
 * and stores, with a byte tail.  It measures short unpredictable branches,
 * byte loads/stores and small-copy code generation.
 *
 * init builds 128 KB of pseudo-text: words from a small dictionary
 * (frequent ones more likely, sentence-initial ones capitalised), decimal
 * numbers, punctuation, phrases repeated from up to 60 KB back (distant
 * matches), now and then a rule line ("-----") or a run of padding (offset-1
 * matches, up to 1 KB, with 255-extended match lengths) and a few random
 * binary bytes or a longer random blob (long literal runs with 255-extended
 * lengths).  It then compresses the text with a greedy LZ4 block encoder
 * (4096-entry hash of 4-byte sequences in the arena, minimum match 4, 64 KB
 * window, the last 5 bytes always literals and no match starting in the
 * last 12 bytes, as the block format requires): 62098 bytes (2.11:1), 11544
 * sequences, 9.1-byte matches on average, 155 of them overlapping.
 * Hashing, offsets and lengths are byte-stream operations, so the
 * compressed block (and the result) are the same on either byte order.
 *
 * Result: bench_mix fold of the decoded length and of every 16th 32-bit
 * word of the output (assembled little-endian from bytes).  EXACT.
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   OFF_HDR   lz_hdr                    8 B    compressed / raw size
 *   OFF_CMP   uint8_t cmp[LZ_BOUND]     <= 131602 B  (read-only in run)
 *   OFF_OUT   uint8_t out[131072]       128 KB (written by run)
 *   OFF_RAW   uint8_t raw[131072]       128 KB (init only: the text)
 *   OFF_HASH  uint32_t hash[4096]       16 KB  (init only: encoder table)
 */
#include "bench_kernel.h"

#define LZ_RAW_BYTES 131072u
#define LZ_MINMATCH  4u
#define LZ_LASTLIT   5u   /* the last 5 bytes of a block are literals */
#define LZ_MFLIMIT   12u  /* no match starts in the last 12 bytes */
#define LZ_MAXDIST   65535u
#define LZ_HASH_LOG  12u
#define LZ_HASH_SIZE (1u << LZ_HASH_LOG)
#define LZ_NO_POS    0xFFFFFFFFu
/* LZ4_COMPRESSBOUND: worst case for incompressible input. */
#define LZ_BOUND     (LZ_RAW_BYTES + LZ_RAW_BYTES / 255u + 16u)
#define LZ_FOLD_STEP 64u

#define OFF_HDR     0u
#define OFF_CMP     BENCH_LINE
#define OFF_OUT     (OFF_CMP + BENCH_ALIGN_UP(LZ_BOUND, BENCH_LINE))
#define OFF_RAW     (OFF_OUT + LZ_RAW_BYTES)
#define OFF_HASH    (OFF_RAW + LZ_RAW_BYTES)
#define ARENA_BYTES (OFF_HASH + LZ_HASH_SIZE * 4u)

typedef struct lz_hdr {
	uint32_t cmp_bytes; /* size of the compressed block */
	uint32_t raw_bytes; /* size of the original text */
} lz_hdr;

/* ---- decoder (run) ------------------------------------------------------ */

/* Copy 8 bytes that do not overlap. */
static inline void lz_copy8(uint8_t *d, const uint8_t *s)
{
	uint8_t b0 = s[0], b1 = s[1], b2 = s[2], b3 = s[3];
	uint8_t b4 = s[4], b5 = s[5], b6 = s[6], b7 = s[7];

	d[0] = b0;
	d[1] = b1;
	d[2] = b2;
	d[3] = b3;
	d[4] = b4;
	d[5] = b5;
	d[6] = b6;
	d[7] = b7;
}

/* Decode one LZ4 block of src_len bytes into dst (capacity dst_cap).
 * Returns the decoded size, or -1 if the block is malformed. */
static int32_t lz4_decode_block(const uint8_t *src, uint32_t src_len,
				uint8_t *dst, uint32_t dst_cap)
{
	const uint8_t *ip = src;
	const uint8_t *const iend = src + src_len;
	uint8_t *op = dst;
	uint8_t *const oend = dst + dst_cap;

	for (;;) {
		const uint8_t *m;
		uint32_t token, len, off, b;

		if (ip >= iend)
			return -1;
		token = *ip++;

		/* Literal run. */
		len = token >> 4;
		if (len == 15) {
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				len += b;
			} while (b == 255);
		}
		if (len > (uint32_t)(iend - ip) || len > (uint32_t)(oend - op))
			return -1;
		for (; len >= 8; len -= 8, op += 8, ip += 8)
			lz_copy8(op, ip);
		for (; len; len--)
			*op++ = *ip++;

		/* The last sequence is literals only. */
		if (ip == iend)
			break;

		/* Match: offset, then the length. */
		if (iend - ip < 2)
			return -1;
		off = (uint32_t)ip[0] | (uint32_t)ip[1] << 8;
		ip += 2;
		if (off == 0 || off > (uint32_t)(op - dst))
			return -1;
		len = token & 15u;
		if (len == 15) {
			do {
				if (ip >= iend)
					return -1;
				b = *ip++;
				len += b;
			} while (b == 255);
		}
		len += LZ_MINMATCH;
		if (len > (uint32_t)(oend - op))
			return -1;
		m = op - off;
		if (off < 8) {
			/* Overlapping: the copy re-reads bytes it has just written. */
			do
				*op++ = *m++;
			while (--len);
		} else {
			for (; len >= 8; len -= 8, op += 8, m += 8)
				lz_copy8(op, m);
			for (; len; len--)
				*op++ = *m++;
		}
	}
	return (int32_t)(op - dst);
}

/* ---- test data and encoder (init) --------------------------------------- */

#define LZ_WORD(s) { s, sizeof(s) - 1 }

static const struct lz_word {
	char s[12];
	uint8_t len;
} lz_dict[64] = {
	LZ_WORD("the"),      LZ_WORD("of"),       LZ_WORD("and"),
	LZ_WORD("to"),       LZ_WORD("in"),       LZ_WORD("a"),
	LZ_WORD("is"),       LZ_WORD("that"),     LZ_WORD("for"),
	LZ_WORD("it"),       LZ_WORD("with"),     LZ_WORD("as"),
	LZ_WORD("was"),      LZ_WORD("on"),       LZ_WORD("be"),
	LZ_WORD("by"),       LZ_WORD("this"),     LZ_WORD("from"),
	LZ_WORD("at"),       LZ_WORD("or"),       LZ_WORD("not"),
	LZ_WORD("are"),      LZ_WORD("which"),    LZ_WORD("an"),
	LZ_WORD("but"),      LZ_WORD("have"),     LZ_WORD("all"),
	LZ_WORD("can"),      LZ_WORD("will"),     LZ_WORD("one"),
	LZ_WORD("each"),     LZ_WORD("when"),     LZ_WORD("frame"),
	LZ_WORD("buffer"),   LZ_WORD("texture"),  LZ_WORD("vertex"),
	LZ_WORD("shader"),   LZ_WORD("player"),   LZ_WORD("level"),
	LZ_WORD("memory"),   LZ_WORD("thread"),   LZ_WORD("vector"),
	LZ_WORD("render"),   LZ_WORD("sound"),    LZ_WORD("stream"),
	LZ_WORD("kernel"),   LZ_WORD("packet"),   LZ_WORD("console"),
	LZ_WORD("address"),  LZ_WORD("pointer"),  LZ_WORD("register"),
	LZ_WORD("instruction"), LZ_WORD("pipeline"), LZ_WORD("controller"),
	LZ_WORD("network"),  LZ_WORD("display"),  LZ_WORD("version"),
	LZ_WORD("system"),   LZ_WORD("function"), LZ_WORD("pixel"),
	LZ_WORD("sector"),   LZ_WORD("cache"),    LZ_WORD("xenon"),
	LZ_WORD("value"),
};

static const char lz_punct[7][3] = {
	". ", ", ", "; ", ": ", ".\n", "? ", "! ",
};

static const char lz_rule[4] = { '-', '=', '*', '.' };

static inline uint32_t lz_put(uint8_t *t, uint32_t pos, uint32_t c)
{
	if (pos < LZ_RAW_BYTES)
		t[pos] = (uint8_t)c;
	return pos + 1;
}

static void lz_make_text(uint8_t *t, bench_rng *r)
{
	uint32_t pos = 0, cap = 1, i;

	while (pos < LZ_RAW_BYTES) {
		uint32_t ev = bench_rng_below(r, 1000);

		if (ev < 740) {
			/* A word; the product of two uniforms favours the
			 * common words at the front of the dictionary. */
			uint32_t k = (bench_rng_below(r, 64) * bench_rng_below(r, 64)) >> 6;
			const struct lz_word *w = &lz_dict[k];

			for (i = 0; i < w->len; i++) {
				uint32_t c = (uint8_t)w->s[i];

				if (i == 0 && cap)
					c -= 'a' - 'A';
				pos = lz_put(t, pos, c);
			}
			pos = lz_put(t, pos, ' ');
			cap = 0;
		} else if (ev < 830) {
			/* A decimal number. */
			uint32_t v = bench_rng_below(r, 100000);
			uint8_t d[5];
			int n = 0;

			do {
				d[n++] = (uint8_t)('0' + v % 10u);
				v /= 10u;
			} while (v);
			while (n)
				pos = lz_put(t, pos, d[--n]);
			pos = lz_put(t, pos, ' ');
		} else if (ev < 920) {
			/* Punctuation replaces the space after the last word. */
			const char *p = lz_punct[bench_rng_below(r, 7)];

			if (pos > 0 && pos <= LZ_RAW_BYTES && t[pos - 1] == ' ')
				pos--;
			pos = lz_put(t, pos, (uint8_t)p[0]);
			pos = lz_put(t, pos, (uint8_t)p[1]);
			cap = p[0] != ',' && p[0] != ';' && p[0] != ':';
		} else if (ev < 970) {
			/* A phrase that already occurred 64 B .. 60 KB ago. */
			uint32_t dist = 64 + bench_rng_below(r, 61376);
			uint32_t len = 12 + bench_rng_below(r, 52);

			for (i = 0; i < len && pos >= dist && pos < LZ_RAW_BYTES; i++)
				pos = lz_put(t, pos, t[pos - dist]);
		} else if (ev < 978) {
			/* A rule line: one character repeated. */
			uint32_t len = 8 + bench_rng_below(r, 100);
			uint32_t c = (uint8_t)lz_rule[bench_rng_below(r, 4)];

			pos = lz_put(t, pos, '\n');
			for (i = 0; i < len; i++)
				pos = lz_put(t, pos, c);
			pos = lz_put(t, pos, '\n');
			cap = 1;
		} else if (ev < 980) {
			/* Padding: a long run of spaces or zero bytes. */
			uint32_t len = 64 + bench_rng_below(r, 960);
			uint32_t c = bench_rng_below(r, 2) ? ' ' : 0;

			for (i = 0; i < len; i++)
				pos = lz_put(t, pos, c);
		} else {
			/* Random bytes: usually a few, now and then a blob. */
			uint32_t len = bench_rng_below(r, 16) == 0
					       ? 200 + bench_rng_below(r, 400)
					       : 1 + bench_rng_below(r, 24);

			for (i = 0; i < len; i++)
				pos = lz_put(t, pos, bench_rng_next(r) >> 24);
		}
	}
}

/* LZ4 hashes 4 bytes read little-endian. */
static inline uint32_t lz_read32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static inline uint32_t lz_hash4(uint32_t v)
{
	return (v * 2654435761u) >> (32 - LZ_HASH_LOG);
}

/* Write the extra length bytes of a length field that is >= 15. */
static uint8_t *lz_put_len(uint8_t *op, uint32_t len)
{
	for (len -= 15; len >= 255; len -= 255)
		*op++ = 255;
	*op++ = (uint8_t)len;
	return op;
}

/* Token, extended literal length and the literals themselves. */
static uint8_t *lz_put_literals(uint8_t *op, uint8_t **token,
				const uint8_t *lit, uint32_t n)
{
	uint32_t i;

	*token = op++;
	if (n >= 15) {
		**token = 15u << 4;
		op = lz_put_len(op, n);
	} else {
		**token = (uint8_t)(n << 4);
	}
	for (i = 0; i < n; i++)
		op[i] = lit[i];
	return op + n;
}

/* Greedy LZ4 block encoder.  Returns the compressed size (at most
 * LZ_BOUND for n bytes of input). */
static uint32_t lz4_encode_block(const uint8_t *src, uint32_t n, uint8_t *dst,
				 uint32_t *tab)
{
	uint32_t ip = 0, anchor = 0;
	uint8_t *op = dst, *token;

	bench_fill(tab, LZ_NO_POS, LZ_HASH_SIZE * 4u);
	while (n > LZ_MFLIMIT && ip <= n - LZ_MFLIMIT) {
		const uint32_t seq = lz_read32(src + ip);
		const uint32_t h = lz_hash4(seq);
		const uint32_t ref = tab[h];
		uint32_t len, lim, off;

		tab[h] = ip;
		if (ref == LZ_NO_POS || ip - ref > LZ_MAXDIST ||
		    lz_read32(src + ref) != seq) {
			ip++;
			continue;
		}
		/* Extend; the match must end before the last 5 bytes. */
		lim = n - LZ_LASTLIT;
		for (len = LZ_MINMATCH; ip + len < lim; len++)
			if (src[ref + len] != src[ip + len])
				break;

		op = lz_put_literals(op, &token, src + anchor, ip - anchor);
		off = ip - ref;
		*op++ = (uint8_t)off;
		*op++ = (uint8_t)(off >> 8);
		if (len - LZ_MINMATCH >= 15) {
			*token |= 15u;
			op = lz_put_len(op, len - LZ_MINMATCH);
		} else {
			*token |= (uint8_t)(len - LZ_MINMATCH);
		}
		ip += len;
		anchor = ip;
	}
	op = lz_put_literals(op, &token, src + anchor, n - anchor);
	return (uint32_t)(op - dst);
}

static void lz4dec_init(void *arena)
{
	lz_hdr *hdr = bench_at(arena, OFF_HDR);
	uint8_t *raw = bench_at(arena, OFF_RAW);
	bench_rng r;

	bench_rng_seed(&r, 0x124DEC05u);
	lz_make_text(raw, &r);
	hdr->raw_bytes = LZ_RAW_BYTES;
	hdr->cmp_bytes = lz4_encode_block(raw, LZ_RAW_BYTES,
					  bench_at(arena, OFF_CMP),
					  bench_at(arena, OFF_HASH));
}

static double lz4dec_run(void *arena)
{
	const lz_hdr *hdr = bench_at(arena, OFF_HDR);
	const uint8_t *cmp = bench_at(arena, OFF_CMP);
	uint8_t *out = bench_at(arena, OFF_OUT);
	int32_t n = lz4_decode_block(cmp, hdr->cmp_bytes, out, LZ_RAW_BYTES);
	uint32_t h = bench_mix(0, (uint32_t)n), i;

	for (i = 0; n > 0 && i + 4 <= (uint32_t)n; i += LZ_FOLD_STEP)
		h = bench_mix(h, lz_read32(out + i));
	return bench_result_u32(h);
}

BENCH_KDEF(lz4dec) = {
	.name = "lz4dec", .desc = "LZ4 block decode, 128 KB text", .unit = "B",
	.category = BENCH_CAT_MIX, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = LZ_RAW_BYTES, .arena_bytes = ARENA_BYTES,
	.init = lz4dec_init, .run = lz4dec_run,
};
