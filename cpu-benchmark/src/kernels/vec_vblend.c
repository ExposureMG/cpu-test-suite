/*
 * vec_vblend.c - VEC: AltiVec RGBA8 alpha blend with per-pixel alpha.
 *
 * out = src over dst for 256x256 RGBA8 images (bytes R, G, B, A in memory),
 * alpha taken from src, written to a separate output image:
 *
 *     out_c = round((src_c * a + dst_c * (255 - a)) / 255)   (c = R, G, B, A)
 *
 * with the exact divide-by-255 trick: t = x + 128, out = (t + (t >> 8)) >> 8,
 * which equals round(x / 255) for every x in [0, 255 * 255].
 *
 * The Xenon VMX unit has no integer multiply (vmuleub/vmuloub, vmladduhm,
 * vmsum*, and also the vsum4* sum-across instructions are missing:
 * testxenon3 rejects vec_mule/vec_mulo/vec_mladd/vec_sum4s as "not
 * implemented by the Xenon PPE", and test16old's patterns for them are
 * broken).  So the products are done the way Xbox 360 code does colour
 * math: in float.  Per 16 bytes (4 pixels):
 *   - unpack the bytes of src and dst to 32-bit lanes (vec_mergeh/vec_mergel
 *     with zero, bytes -> halfwords -> words), one pixel per vector;
 *   - convert to float (vec_ctf), splat alpha (word 3) with vec_perm (both
 *     compilers turn it into one vspltw; vec_splat of a computed vector
 *     float is lowered by both through the stack and a GPR, i.e. two
 *     load-hit-stores per pixel);
 *   - x + 128 = (s - d) * a + (255 * d + 128) with two vec_madd: every
 *     intermediate is an integer below 2^24, so the float math is exact;
 *   - back to u32 (vec_ctu), pack to u16 (vec_pack: values <= 65153), then
 *     the rounding divide in 16-bit lanes (vec_sr, vec_add: t + (t >> 8) <=
 *     65407 never wraps) and vec_packsu to bytes.
 * The output is folded on the fly: per u32 lane a rotate-xor hash (vec_rl,
 * vec_xor) and a modular sum (vec_add), combined with bench_mix at the end.
 *
 * Host build (no <altivec.h> on x86): the #else branch does the same
 * computation with integer arithmetic, the same data layout, and builds the
 * u32 lanes from the output bytes big-endian (as vector unsigned int does
 * on the target), so its result is bit-identical to the AltiVec one.
 *
 * Working set: 3 x 256 KB = 768 KB (src, dst, separate output), above the
 * 512 KB guideline but inside the 1 MB L2; the image size is fixed by the
 * px / 65536 work unit.  Result: EXACT.
 */
#include "bench_kernel.h"

#ifdef __ALTIVEC__
#include <altivec.h>
#endif

#define IMG_W 256u
#define IMG_H 256u
#define NPIX (IMG_W * IMG_H)
#define IMG_BYTES (NPIX * 4u) /* 256 KB */
#define OFF_SRC 0u
#define OFF_DST (OFF_SRC + IMG_BYTES)
#define OFF_OUT (OFF_DST + IMG_BYTES)
#define ARENA_BYTES (OFF_OUT + IMG_BYTES)

static void vblend_init(void *arena)
{
	unsigned char *src = bench_at(arena, OFF_SRC);
	unsigned char *dst = bench_at(arena, OFF_DST);
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0x76626c64u); /* "vbld" */
	for (i = 0; i < NPIX; i++) {
		const uint32_t c = bench_rng_next(&r);
		const uint32_t k = bench_rng_below(&r, 8);

		src[4 * i + 0] = (unsigned char)c;
		src[4 * i + 1] = (unsigned char)(c >> 8);
		src[4 * i + 2] = (unsigned char)(c >> 16);
		/* Sprite-like alpha: 1/4 transparent, 1/4 opaque, the rest
		 * partial (anti-aliased edges, smoke, glass). */
		src[4 * i + 3] = k < 2 ? 0 : k < 4 ? 255 : (unsigned char)(c >> 24);
	}
	for (i = 0; i < NPIX; i++) {
		const uint32_t c = bench_rng_next(&r);

		dst[4 * i + 0] = (unsigned char)c;
		dst[4 * i + 1] = (unsigned char)(c >> 8);
		dst[4 * i + 2] = (unsigned char)(c >> 16);
		dst[4 * i + 3] = (unsigned char)(c >> 24);
	}
}

#ifdef __ALTIVEC__

/* One pixel, R G B A in the 4 words of s and d: s * a + d * (255 - a) + 128,
 * exact in float (all values are integers below 2^24). */
static inline vector unsigned int blend_px(vector unsigned int s,
					   vector unsigned int d)
{
	const vector unsigned char alpha = { 12, 13, 14, 15, 12, 13, 14, 15,
					     12, 13, 14, 15, 12, 13, 14, 15 };
	const vector float f255 = { 255.0f, 255.0f, 255.0f, 255.0f };
	const vector float f128 = { 128.0f, 128.0f, 128.0f, 128.0f };
	const vector float sf = vec_ctf(s, 0);
	const vector float df = vec_ctf(d, 0);
	const vector float af = vec_perm(sf, sf, alpha); /* A A A A */

	return vec_ctu(vec_madd(vec_sub(sf, df), af, vec_madd(df, f255, f128)), 0);
}

/* (t + (t >> 8)) >> 8 per halfword: round(x / 255) for t = x + 128. */
static inline vector unsigned short div255(vector unsigned short t)
{
	const vector unsigned short sh8 = vec_splat_u16(8);

	return vec_sr(vec_add(t, vec_sr(t, sh8)), sh8);
}

/* Blend 4 pixels (16 bytes). */
static inline vector unsigned char blend4(vector unsigned char s,
					  vector unsigned char d)
{
	const vector unsigned char z8 = vec_splat_u8(0);
	const vector unsigned short z16 = vec_splat_u16(0);
	const vector unsigned short sh = (vector unsigned short)vec_mergeh(z8, s);
	const vector unsigned short sl = (vector unsigned short)vec_mergel(z8, s);
	const vector unsigned short dh = (vector unsigned short)vec_mergeh(z8, d);
	const vector unsigned short dl = (vector unsigned short)vec_mergel(z8, d);
	const vector unsigned int t0 = blend_px(
		(vector unsigned int)vec_mergeh(z16, sh),
		(vector unsigned int)vec_mergeh(z16, dh));
	const vector unsigned int t1 = blend_px(
		(vector unsigned int)vec_mergel(z16, sh),
		(vector unsigned int)vec_mergel(z16, dh));
	const vector unsigned int t2 = blend_px(
		(vector unsigned int)vec_mergeh(z16, sl),
		(vector unsigned int)vec_mergeh(z16, dl));
	const vector unsigned int t3 = blend_px(
		(vector unsigned int)vec_mergel(z16, sl),
		(vector unsigned int)vec_mergel(z16, dl));

	return vec_packsu(div255(vec_pack(t0, t1)), div255(vec_pack(t2, t3)));
}

static double vblend_run(void *arena)
{
	const unsigned char *src = bench_at(arena, OFF_SRC);
	const unsigned char *dst = bench_at(arena, OFF_DST);
	unsigned char *out = bench_at(arena, OFF_OUT);
	const vector unsigned int rot1 = vec_splat_u32(1);
	vector unsigned int hx = vec_splat_u32(0), hs = hx;
	union {
		vector unsigned int v;
		uint32_t u[4];
	} fx, fs;
	uint32_t i, h = 0;

	for (i = 0; i < IMG_BYTES; i += 16) {
		const vector unsigned char o = blend4(vec_ld(i, src), vec_ld(i, dst));
		const vector unsigned int w = (vector unsigned int)o;

		vec_st(o, i, out);
		hx = vec_xor(vec_rl(hx, rot1), w);
		hs = vec_add(hs, w);
	}
	fx.v = hx;
	fs.v = hs;
	for (i = 0; i < 4; i++)
		h = bench_mix(h, fx.u[i]);
	for (i = 0; i < 4; i++)
		h = bench_mix(h, fs.u[i]);
	return bench_result_u32(h);
}

#else /* !__ALTIVEC__: scalar version for the host build */

static double vblend_run(void *arena)
{
	const unsigned char *src = bench_at(arena, OFF_SRC);
	const unsigned char *dst = bench_at(arena, OFF_DST);
	unsigned char *out = bench_at(arena, OFF_OUT);
	uint32_t hx[4] = { 0, 0, 0, 0 }, hs[4] = { 0, 0, 0, 0 };
	uint32_t i, k, c, h = 0;

	for (i = 0; i < IMG_BYTES; i += 16) {
		const unsigned char *s = src + i;
		const unsigned char *d = dst + i;
		unsigned char *o = out + i;

		for (c = 0; c < 16; c++) {
			const uint32_t a = s[c | 3u]; /* alpha of this pixel */
			const uint32_t t = s[c] * a + d[c] * (255u - a) + 128u;

			o[c] = (unsigned char)((t + (t >> 8)) >> 8);
		}
		for (k = 0; k < 4; k++) {
			/* the lane value vector unsigned int gives on big-endian */
			const uint32_t w = (uint32_t)o[4 * k] << 24 |
					   (uint32_t)o[4 * k + 1] << 16 |
					   (uint32_t)o[4 * k + 2] << 8 |
					   (uint32_t)o[4 * k + 3];

			hx[k] = ((hx[k] << 1) | (hx[k] >> 31)) ^ w;
			hs[k] += w;
		}
	}
	for (k = 0; k < 4; k++)
		h = bench_mix(h, hx[k]);
	for (k = 0; k < 4; k++)
		h = bench_mix(h, hs[k]);
	return bench_result_u32(h);
}

#endif /* __ALTIVEC__ */

BENCH_KDEF(vblend) = {
	.name = "vblend", .desc = "256x256 RGBA8 src-over, per-px a", .unit = "px",
	.category = BENCH_CAT_VEC, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = NPIX, .arena_bytes = ARENA_BYTES,
	.init = vblend_init, .run = vblend_run,
};
