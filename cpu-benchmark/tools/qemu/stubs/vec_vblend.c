/* qemu stub for vblend: AltiVec intrinsics on bytes (vec_perm alpha splat,
 * merges, int<->float conversions, vec_madd, shifts, vec_packsu), scalar
 * fallback of the same layout.  EXACT. */
#include "bench_kernel.h"
#ifdef __ALTIVEC__
#include <altivec.h>
#endif

#define NPX 16384u
#define SRC_OFF 0u
#define DST_OFF (NPX * 4u)
#define OUT_OFF (2u * NPX * 4u)

static void vblend_init(void *arena)
{
	unsigned char *s = (unsigned char *)bench_at(arena, SRC_OFF);
	unsigned char *d = (unsigned char *)bench_at(arena, DST_OFF);
	bench_rng r;
	uint32_t i;

	/* a byte stream (RGBA8): generate bytes, not native words */
	bench_rng_seed(&r, 0xb1e0du);
	for (i = 0; i < NPX * 4u; i++) {
		s[i] = (unsigned char)(bench_rng_next(&r) >> 24);
		d[i] = (unsigned char)(bench_rng_next(&r) >> 24);
	}
}

static inline uint32_t div255(uint32_t x)
{
	return (x + 128u + ((x + 128u) >> 8)) >> 8;
}

static double vblend_run(void *arena)
{
	const unsigned char *s = (const unsigned char *)bench_at(arena, SRC_OFF);
	const unsigned char *d = (const unsigned char *)bench_at(arena, DST_OFF);
	unsigned char *o = (unsigned char *)bench_at(arena, OUT_OFF);
	uint32_t i, h = 0;
#ifdef __ALTIVEC__
	/* Xenon's VMX has no integer multiplies (testxenon3 rejects vec_mule /
	 * vec_mulo / vec_mladd, test16old ICEs on them), so: widen the bytes to
	 * 32-bit lanes, blend in float (exact: products < 2^24), back to int,
	 * rounding divide by 255, pack with saturation. */
	const vector unsigned char asplat = { 3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15 };
	const vector unsigned char ff = vec_splat_u8(-1), z8 = vec_splat_u8(0);
	const vector unsigned short z16 = vec_splat_u16(0);
	const vector unsigned int c8 = vec_splat_u32(8);
	const vector unsigned int c128 = vec_sl(vec_splat_u32(1), vec_splat_u32(7));
	const vector float zf = (vector float)vec_splat_u32(0);

	for (i = 0; i < NPX * 4u; i += 16u) {
		vector unsigned char vs = vec_ld(0, s + i), vd = vec_ld(0, d + i);
		vector unsigned char a = vec_perm(vs, vs, asplat), ia = vec_sub(ff, a);
		vector unsigned char in[4] = { vs, vd, a, ia };
		vector signed int w[4][4];
		vector unsigned int r[4];
		int k, j;

		for (k = 0; k < 4; k++) {
			vector unsigned short h = (vector unsigned short)vec_mergeh(z8, in[k]);
			vector unsigned short l = (vector unsigned short)vec_mergel(z8, in[k]);

			w[k][0] = (vector signed int)vec_mergeh(z16, h);
			w[k][1] = (vector signed int)vec_mergel(z16, h);
			w[k][2] = (vector signed int)vec_mergeh(z16, l);
			w[k][3] = (vector signed int)vec_mergel(z16, l);
		}
		for (j = 0; j < 4; j++) {
			vector float x = vec_madd(vec_ctf(w[0][j], 0), vec_ctf(w[2][j], 0),
						  vec_madd(vec_ctf(w[1][j], 0), vec_ctf(w[3][j], 0), zf));
			vector unsigned int t = vec_add((vector unsigned int)vec_cts(x, 0), c128);

			r[j] = vec_sr(vec_add(t, vec_sr(t, c8)), c8);
		}
		vec_st(vec_packsu(vec_packsu(r[0], r[1]), vec_packsu(r[2], r[3])), 0, o + i);
	}
#else
	for (i = 0; i < NPX * 4u; i++) {
		uint32_t a = s[(i & ~3u) + 3u];

		o[i] = (unsigned char)div255(s[i] * a + d[i] * (255u - a));
	}
#endif
	for (i = 0; i < NPX * 4u; i += 4u)
		h = bench_mix(h, (uint32_t)o[i] << 24 | (uint32_t)o[i + 1] << 16 | (uint32_t)o[i + 2] << 8 | o[i + 3]);
	return bench_result_u32(h);
}

BENCH_KDEF(vblend) = {
	.name = "vblend", .desc = "qemu stub: AltiVec integer", .unit = "px",
	.category = BENCH_CAT_VEC, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = NPX, .arena_bytes = OUT_OFF + NPX * 4u,
	.init = vblend_init, .run = vblend_run,
};
