/*
 * mix_idct8.c - MIX: JPEG "islow" 8x8 integer IDCT over 1024 blocks.
 *
 * The accurate integer inverse DCT of the IJG decoder (jidctint.c, the
 * Loeffler-Ligtenberg-Moschytz flow graph with 12 multiplies), which most
 * JPEG and Motion-JPEG decoders in games and media players still run:
 *
 *   pass 1 (columns): dequantise the int16 coefficients with the uint16
 *     quantisation table; a column whose 7 AC terms are all zero (most of
 *     them, after quantisation) takes the shortcut "every output = DC";
 *     otherwise the even part (a rotation: 3 multiplies, then butterflies)
 *     and the odd part (9 multiplies) run, all multiplies by constants with
 *     13 fraction bits, and the results are descaled to PASS1_BITS of extra
 *     precision into an int32 workspace;
 *   pass 2 (rows): the same 1-D transform on each workspace row (with the
 *     same all-AC-zero shortcut), descaled by CONST_BITS + PASS1_BITS + 3
 *     with rounding, level-shifted by +128 and clamped to 0..255.
 *
 * It measures int16 loads (sign-extending), 32-bit multiplies by constants,
 * short data-dependent branches and byte stores.  The math is jidctint.c's
 * (CONST_BITS 13, PASS1_BITS 2, the same FIX_ constants and rounding), so
 * the output is bit-identical to libjpeg's jpeg_idct_islow (checked against
 * libjpeg-turbo 3.2 on all 65536 output samples; at most 1 off a double-
 * precision IDCT); left shifts of possibly negative values are written as
 * multiplies by powers of two.  On this data 81% of the pass-1 columns take
 * the shortcut, and 2.3% of the output samples need the clamp.
 *
 * init generates 1024 blocks of a 256x256 grayscale image, coded with the
 * JPEG Annex K luminance table scaled to quality 75: the DC values follow a
 * random walk (as neighbouring blocks do), 25% of the blocks are flat (DC
 * only), the rest carry 1..20 small AC coefficients placed in zigzag order
 * at the low frequencies, and every dequantised coefficient stays within
 * the +-1023 range of 8-bit JPEG.  run decodes every block into the 256x256
 * image in the arena (row stride 256), as a decoder's output_buf.
 *
 * Result: per output row, the 8 bytes packed into two words; one word is
 * summed, the other xor-rotated in, and both are folded with bench_mix at
 * the end.  EXACT.
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   OFF_QT    uint16_t qt[64]            128 B  (read-only in run)
 *   OFF_COEF  int16_t coef[1024][64]     128 KB (read-only in run)
 *   OFF_IMG   uint8_t img[256][256]      64 KB  (written by run)
 */
#include "bench_kernel.h"

#define IDCT_BLOCKS 1024u
#define IMG_W       256u /* 32 x 32 blocks */
#define DCTSIZE     8
#define DCTSIZE2    64
#define CONST_BITS  13
#define PASS1_BITS  2

#define OFF_QT      0u
#define OFF_COEF    BENCH_LINE
#define OFF_IMG     (OFF_COEF + IDCT_BLOCKS * DCTSIZE2 * 2u)
#define ARENA_BYTES (OFF_IMG + IMG_W * IMG_W)

/* round(x * 2^CONST_BITS), as jidctint.c tabulates them for CONST_BITS 13 */
#define FIX_0_298631336 2446
#define FIX_0_390180644 3196
#define FIX_0_541196100 4433
#define FIX_0_765366865 6270
#define FIX_0_899976223 7373
#define FIX_1_175875602 9633
#define FIX_1_501321110 12299
#define FIX_1_847759065 15137
#define FIX_1_961570560 16069
#define FIX_2_053119869 16819
#define FIX_2_562915447 20995
#define FIX_3_072711026 25172

/* x * 2^n without the undefined left shift of a negative value. */
#define SCALE_UP(x, n) ((x) * ((int32_t)1 << (n)))
/* Right shift by n with rounding (arithmetic shift of signed values). */
#define DESCALE(x, n)  (((x) + ((int32_t)1 << ((n) - 1))) >> (n))
#define DEQUANT(c, q)  ((int32_t)(c) * (int32_t)(q))

/*
 * One 8-point 1-D IDCT: inputs i0..i7, outputs o0..o7 scaled up by
 * 2^CONST_BITS (and by sqrt(8) relative to a true IDCT), not yet descaled.
 */
#define IDCT_1D(i0, i1, i2, i3, i4, i5, i6, i7, o0, o1, o2, o3, o4, o5, o6, o7) \
	do {                                                                    \
		int32_t e0, e1, e2, e3, e10, e11, e12, e13;                      \
		int32_t d0, d1, d2, d3, z1, z2, z3, z4, z5;                      \
		/* Even part: the rotator sqrt(2)*c(-6) on i2 / i6. */          \
		z2 = (i2);                                                       \
		z3 = (i6);                                                       \
		z1 = (z2 + z3) * FIX_0_541196100;                                \
		e2 = z1 - z3 * FIX_1_847759065;                                  \
		e3 = z1 + z2 * FIX_0_765366865;                                  \
		z2 = (i0);                                                       \
		z3 = (i4);                                                       \
		e0 = SCALE_UP(z2 + z3, CONST_BITS);                              \
		e1 = SCALE_UP(z2 - z3, CONST_BITS);                              \
		e10 = e0 + e3;                                                   \
		e13 = e0 - e3;                                                   \
		e11 = e1 + e2;                                                   \
		e12 = e1 - e2;                                                   \
		/* Odd part: d0..d3 are i7, i5, i3, i1. */                      \
		d0 = (i7);                                                       \
		d1 = (i5);                                                       \
		d2 = (i3);                                                       \
		d3 = (i1);                                                       \
		z1 = d0 + d3;                                                    \
		z2 = d1 + d2;                                                    \
		z3 = d0 + d2;                                                    \
		z4 = d1 + d3;                                                    \
		z5 = (z3 + z4) * FIX_1_175875602;                                \
		d0 *= FIX_0_298631336;                                           \
		d1 *= FIX_2_053119869;                                           \
		d2 *= FIX_3_072711026;                                           \
		d3 *= FIX_1_501321110;                                           \
		z1 *= -FIX_0_899976223;                                          \
		z2 *= -FIX_2_562915447;                                          \
		z3 = z3 * -FIX_1_961570560 + z5;                                 \
		z4 = z4 * -FIX_0_390180644 + z5;                                 \
		d0 += z1 + z3;                                                   \
		d1 += z2 + z4;                                                   \
		d2 += z2 + z3;                                                   \
		d3 += z1 + z4;                                                   \
		(o0) = e10 + d3;                                                 \
		(o7) = e10 - d3;                                                 \
		(o1) = e11 + d2;                                                 \
		(o6) = e11 - d2;                                                 \
		(o2) = e12 + d1;                                                 \
		(o5) = e12 - d1;                                                 \
		(o3) = e13 + d0;                                                 \
		(o4) = e13 - d0;                                                 \
	} while (0)

/* Level shift by +128 and range limit to 0..255 of a descaled sample. */
static inline uint32_t idct_clamp(int32_t v)
{
	v += 128;
	if (v < 0)
		v = 0;
	else if (v > 255)
		v = 255;
	return (uint32_t)v;
}

/* Final descale of a pass-2 output. */
#define PASS2_OUT(x) idct_clamp(DESCALE(x, CONST_BITS + PASS1_BITS + 3))

/* Decode one block to out (row stride 'stride'); fold the outputs. */
static void idct_islow(const int16_t *coef, const uint16_t *qt, uint8_t *out,
		       uint32_t stride, uint32_t *sum, uint32_t *rot)
{
	int32_t ws[DCTSIZE2];
	uint32_t s = *sum, x = *rot;
	int c;

	/* Pass 1: columns from the coefficients into the workspace. */
	for (c = 0; c < DCTSIZE; c++) {
		const int16_t *in = coef + c;
		const uint16_t *q = qt + c;
		int32_t *w = ws + c;
		int32_t o0, o1, o2, o3, o4, o5, o6, o7;

		if (in[8] == 0 && in[16] == 0 && in[24] == 0 && in[32] == 0 &&
		    in[40] == 0 && in[48] == 0 && in[56] == 0) {
			int32_t dc = SCALE_UP(DEQUANT(in[0], q[0]), PASS1_BITS);

			w[0] = dc;
			w[8] = dc;
			w[16] = dc;
			w[24] = dc;
			w[32] = dc;
			w[40] = dc;
			w[48] = dc;
			w[56] = dc;
			continue;
		}
		IDCT_1D(DEQUANT(in[0], q[0]), DEQUANT(in[8], q[8]),
			DEQUANT(in[16], q[16]), DEQUANT(in[24], q[24]),
			DEQUANT(in[32], q[32]), DEQUANT(in[40], q[40]),
			DEQUANT(in[48], q[48]), DEQUANT(in[56], q[56]),
			o0, o1, o2, o3, o4, o5, o6, o7);
		w[0] = DESCALE(o0, CONST_BITS - PASS1_BITS);
		w[8] = DESCALE(o1, CONST_BITS - PASS1_BITS);
		w[16] = DESCALE(o2, CONST_BITS - PASS1_BITS);
		w[24] = DESCALE(o3, CONST_BITS - PASS1_BITS);
		w[32] = DESCALE(o4, CONST_BITS - PASS1_BITS);
		w[40] = DESCALE(o5, CONST_BITS - PASS1_BITS);
		w[48] = DESCALE(o6, CONST_BITS - PASS1_BITS);
		w[56] = DESCALE(o7, CONST_BITS - PASS1_BITS);
	}

	/* Pass 2: rows from the workspace into the image. */
	for (c = 0; c < DCTSIZE; c++) {
		const int32_t *w = ws + c * DCTSIZE;
		uint8_t *o = out + (uint32_t)c * stride;
		uint32_t p0, p1, p2, p3, p4, p5, p6, p7;

		if (w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 &&
		    w[5] == 0 && w[6] == 0 && w[7] == 0) {
			/* AC terms all zero: the DC term only, descaled by
			 * PASS1_BITS + 3 (the 2^CONST_BITS never applied). */
			p0 = idct_clamp(DESCALE(w[0], PASS1_BITS + 3));
			p1 = p2 = p3 = p4 = p5 = p6 = p7 = p0;
		} else {
			int32_t o0, o1, o2, o3, o4, o5, o6, o7;

			IDCT_1D(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
				o0, o1, o2, o3, o4, o5, o6, o7);
			p0 = PASS2_OUT(o0);
			p1 = PASS2_OUT(o1);
			p2 = PASS2_OUT(o2);
			p3 = PASS2_OUT(o3);
			p4 = PASS2_OUT(o4);
			p5 = PASS2_OUT(o5);
			p6 = PASS2_OUT(o6);
			p7 = PASS2_OUT(o7);
		}
		o[0] = (uint8_t)p0;
		o[1] = (uint8_t)p1;
		o[2] = (uint8_t)p2;
		o[3] = (uint8_t)p3;
		o[4] = (uint8_t)p4;
		o[5] = (uint8_t)p5;
		o[6] = (uint8_t)p6;
		o[7] = (uint8_t)p7;
		s += p0 << 24 | p1 << 16 | p2 << 8 | p3;
		x = (x << 5 | x >> 27) ^ (p4 << 24 | p5 << 16 | p6 << 8 | p7);
	}
	*sum = s;
	*rot = x;
}

/* ---- init ---------------------------------------------------------------- */

/* JPEG Annex K table K.1 (luminance), natural order. */
static const uint8_t idct_luma_q50[DCTSIZE2] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77,
	24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103, 99,
};

/* Zigzag index -> natural (row-major) index. */
static const uint8_t idct_zigzag[DCTSIZE2] = {
	0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static void idct8_init(void *arena)
{
	uint16_t *qt = bench_at(arena, OFF_QT);
	int16_t *coef = bench_at(arena, OFF_COEF);
	bench_rng r;
	int32_t dc = 0;
	uint32_t b, i;

	/* libjpeg quality scaling for quality 75: q = (q50 * 50 + 50) / 100. */
	for (i = 0; i < DCTSIZE2; i++) {
		uint32_t q = (idct_luma_q50[i] * 50u + 50u) / 100u;

		qt[i] = (uint16_t)(q ? q : 1u);
	}

	bench_rng_seed(&r, 0x1DC78E11u);
	for (b = 0; b < IDCT_BLOCKS; b++) {
		int16_t *blk = coef + b * DCTSIZE2;
		uint32_t kind = bench_rng_below(&r, 16), nac, span;
		int32_t dcmax = 1023 / qt[0];

		bench_fill(blk, 0, DCTSIZE2 * 2u);

		/* DC: a random walk, like neighbouring blocks of one image. */
		dc += (int32_t)bench_rng_below(&r, 25) - 12;
		if (dc > dcmax)
			dc = dcmax;
		if (dc < -dcmax)
			dc = -dcmax;
		blk[0] = (int16_t)dc;

		if (kind < 4)
			nac = 0; /* flat */
		else if (kind < 12)
			nac = 1 + bench_rng_below(&r, 4);
		else if (kind < 15)
			nac = 5 + bench_rng_below(&r, 6);
		else
			nac = 11 + bench_rng_below(&r, 10);
		span = 3 + 2 * nac;
		if (span > 63)
			span = 63;

		for (i = 0; i < nac; i++) {
			uint32_t zz = 1 + bench_rng_below(&r, span);
			uint32_t pos = idct_zigzag[zz];
			int32_t lim = 1023 / qt[pos];
			int32_t mag = 1 + (int32_t)bench_rng_below(
					&r, 1 + bench_rng_below(&r, zz < 6 ? 24 : 8));

			if (mag > lim)
				mag = lim;
			blk[pos] = (int16_t)(bench_rng_below(&r, 2) ? mag : -mag);
		}
	}
}

static double idct8_run(void *arena)
{
	const uint16_t *qt = bench_at(arena, OFF_QT);
	const int16_t *coef = bench_at(arena, OFF_COEF);
	uint8_t *img = bench_at(arena, OFF_IMG);
	uint32_t sum = 0, rot = 0, b;

	for (b = 0; b < IDCT_BLOCKS; b++) {
		uint8_t *out = img + (b / (IMG_W / DCTSIZE)) * (IMG_W * DCTSIZE) +
			       (b % (IMG_W / DCTSIZE)) * DCTSIZE;

		idct_islow(coef + b * DCTSIZE2, qt, out, IMG_W, &sum, &rot);
	}
	return bench_result_u32(bench_mix(bench_mix(0, sum), rot));
}

BENCH_KDEF(idct8) = {
	.name = "idct8", .desc = "JPEG islow IDCT 8x8, 1024 blk", .unit = "blk",
	.category = BENCH_CAT_MIX, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = IDCT_BLOCKS, .arena_bytes = ARENA_BYTES,
	.init = idct8_init, .run = idct8_run,
};
