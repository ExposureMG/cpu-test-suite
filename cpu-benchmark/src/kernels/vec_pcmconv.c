/*
 * vec_pcmconv.c - VEC: plain-C audio mixer path, int16 stereo -> 2x2 gain
 * matrix -> int16 stereo.
 *
 * 32768 interleaved int16 frames (L, R) are converted to float, run through
 * a 2x2 gain/pan matrix (here a "stereo widener": +1 dB / +2 dB direct, a
 * -9 dB crossfeed into L and a phase-inverted -15 dB crossfeed into R),
 * clamped to the int16 range with explicit compares, rounded half away from
 * zero (add +-0.5, truncate: no libm) and stored as interleaved int16, the
 * way a software mixer feeds a sound buffer.  The mixer also returns the sum
 * of the output samples (a level meter).  The hot loop is exactly what the
 * toolchains are said to differ on: int16 loads (lha vs lhz + extsh),
 * int -> float and float -> int conversions (through memory on PPC: fcfid /
 * fctiwz plus a load-hit-store each), FP compares, and a loop that a
 * vectoriser could turn into vupkhsh / vcfsx / vmaddfp / vsel / vctsxs /
 * vpkswss.
 *
 * Gains are read from the arena (|g| < 1.5).  Input: a band-limited synth
 * signal (two triangle waves per channel, +-1500 of noise and a DC offset:
 * L in [-25458, 30529], R in [-26088, 28155]), so the sign and clip
 * branches see real-audio patterns (the L output changes sign on 3.7% of
 * the frames) instead of white noise; 0.58% of the L and 0.27% of the R
 * outputs clip.  The DC offset keeps the checksum far from zero.
 *
 * Result: the sum of all 65536 output samples, accumulated exactly in an
 * int32 (it cannot overflow: |sum| <= 65536 * 32768 = 2^31 and the positive
 * side is at most 65536 * 32767 < 2^31 - 1) and returned as a double
 * (1.198e8).  APPROX, relative tolerance 5e-6.  The outputs are integers,
 * but l * g00 + r * g01 may legitimately round differently between
 * compilers: contracted to one fmadds (either product unrounded) or not, or
 * vectorised.  That moves the float value by 1-2 ulps (<= 2^-9 each below
 * 32768), which flips the rounded output by +-1 only when the value lies
 * within that distance of a .5 boundary: with this data 156 samples are
 * within 1 ulp and 290 within 2 ulps, so |delta sum| <= 290, i.e. <= 2.4e-6
 * relative even if every one of them flipped the same way (random signs
 * give ~sqrt(156) ~ 12, 1e-7).  Measured on the host (x86 FMA vs
 * -ffp-contract=off): 12 samples differ by 1, delta sum = 2 (1.7e-8).
 * 5e-6 (600 in absolute terms) covers the worst case; a real bug (a wrong
 * gain, a swapped channel, truncation instead of rounding, a broken clamp)
 * changes the sum by 3e4 or more.
 */
#include "bench_kernel.h"

#define FRAMES 32768u
#define PCM_BYTES (FRAMES * 2u * 2u) /* 128 KB */
#define OFF_GAIN 0u                  /* 4 floats, padded to a line */
#define OFF_IN 128u
#define OFF_OUT (OFF_IN + PCM_BYTES)
#define ARENA_BYTES (OFF_OUT + PCM_BYTES)

/* Triangle wave with period per (even) and peak amp, at sample i. */
static int32_t tri_wave(uint32_t i, uint32_t per, int32_t amp)
{
	const int32_t half = (int32_t)(per / 2u);
	const int32_t p = (int32_t)(i % per);
	const int32_t up = p < half ? p : (int32_t)per - p; /* 0..half */

	return (2 * up - half) * amp / half;
}

static void pcmconv_init(void *arena)
{
	float *g = bench_at(arena, OFF_GAIN);
	int16_t *in = bench_at(arena, OFF_IN);
	bench_rng r;
	uint32_t i;

	g[0] = 1.122f;  /* L <- L, +1 dB  */
	g[1] = 0.355f;  /* L <- R, -9 dB  */
	g[2] = -0.178f; /* R <- L, -15 dB, inverted */
	g[3] = 1.259f;  /* R <- R, +2 dB  */
	/* A band-limited synth signal (two triangle waves per channel, a bit
	 * of noise and a DC offset), so the sign and clip branches behave as
	 * with real audio rather than white noise. */
	bench_rng_seed(&r, 0x70636d63u); /* "pcmc" */
	for (i = 0; i < FRAMES; i++) {
		const int32_t nl = (int32_t)bench_rng_below(&r, 3001) - 1500;
		const int32_t nr = (int32_t)bench_rng_below(&r, 3001) - 1500;
		const int32_t l = tri_wave(i, 194, 17000) + tri_wave(i + 17, 62, 10000) +
				  nl + 2500;
		const int32_t rr = tri_wave(i + 50, 226, 16500) +
				   tri_wave(i, 86, 9500) + nr + 800;

		in[2 * i + 0] = (int16_t)l;
		in[2 * i + 1] = (int16_t)rr;
	}
}

/* Clamp to the int16 range and round half away from zero. */
static inline int32_t to_s16(float v)
{
	if (v > 32767.0f)
		v = 32767.0f;
	else if (v < -32768.0f)
		v = -32768.0f;
	return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

/* out = gain * in for interleaved stereo; returns the sum of the output. */
static int32_t mix_2x2(int16_t *restrict out, const int16_t *restrict in,
		       uint32_t frames, const float *restrict gain)
{
	const float g00 = gain[0], g01 = gain[1], g10 = gain[2], g11 = gain[3];
	int32_t level = 0;
	uint32_t i;

	for (i = 0; i < frames; i++) {
		const float l = in[2 * i], r = in[2 * i + 1];
		const int32_t ol = to_s16(l * g00 + r * g01);
		const int32_t or_ = to_s16(l * g10 + r * g11);

		out[2 * i] = (int16_t)ol;
		out[2 * i + 1] = (int16_t)or_;
		level += ol + or_;
	}
	return level;
}

static double pcmconv_run(void *arena)
{
	const float *g = bench_at(arena, OFF_GAIN);
	const int16_t *in = bench_at(arena, OFF_IN);
	int16_t *out = bench_at(arena, OFF_OUT);

	return (double)mix_2x2(out, in, FRAMES, g);
}

BENCH_KDEF(pcmconv) = {
	.name = "pcmconv", .desc = "s16 stereo 2x2 gain, clamp, s16", .unit = "frm",
	.category = BENCH_CAT_VEC, .flags = BENCH_F_APPROX, .tolerance = 5e-6,
	.units = FRAMES, .arena_bytes = ARENA_BYTES,
	.init = pcmconv_init, .run = pcmconv_run,
};
