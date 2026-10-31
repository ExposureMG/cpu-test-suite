/* qemu stub for vxform (used only by tools/qemu/qscratch.sh when the real
 * kernel is missing): AltiVec float intrinsics, splat + vec_madd matrix
 * transform, vec_re + one Newton-Raphson step, with a scalar fallback of the
 * same layout for the host.  APPROX. */
#include "bench_kernel.h"
#ifdef __ALTIVEC__
#include <altivec.h>
#endif

#define NV 2048u
#define IN_OFF 0u
#define OUT_OFF (NV * 16u)
#define MAT_OFF (2u * NV * 16u)

static void vxform_init(void *arena)
{
	float *in = (float *)bench_at(arena, IN_OFF), *m = (float *)bench_at(arena, MAT_OFF);
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0x7f0001u);
	for (i = 0; i < NV * 4u; i++)
		in[i] = bench_rng_f01(&r) * 4.0f - 2.0f;
	for (i = 0; i < NV; i++)
		in[i * 4u + 3u] = 1.0f;
	for (i = 0; i < 16u; i++)
		m[i] = bench_rng_f01(&r) - 0.5f;
	m[15] = 3.0f;
}

static double vxform_run(void *arena)
{
	const float *in = (const float *)bench_at(arena, IN_OFF);
	const float *m = (const float *)bench_at(arena, MAT_OFF);
	float *out = (float *)bench_at(arena, OUT_OFF);
	double sum = 0.0;
	uint32_t i;
#ifdef __ALTIVEC__
	vector float c0 = vec_ld(0, m), c1 = vec_ld(16, m), c2 = vec_ld(32, m), c3 = vec_ld(48, m);
	vector float one = vec_splat(vec_ld(0, in + 3), 3);

	for (i = 0; i < NV; i++) {
		vector float v = vec_ld(0, in + i * 4u);
		vector float x = vec_splat(v, 0), y = vec_splat(v, 1), z = vec_splat(v, 2), w = vec_splat(v, 3);
		vector float t = vec_madd(x, c0, vec_madd(y, c1, vec_madd(z, c2, vec_madd(w, c3, (vector float)vec_splat_u32(0)))));
		vector float ww = vec_splat(t, 3);
		vector float e = vec_re(ww);
		e = vec_madd(e, vec_nmsub(e, ww, one), e); /* Newton-Raphson */
		vec_st(vec_madd(t, e, (vector float)vec_splat_u32(0)), 0, out + i * 4u);
	}
#else
	for (i = 0; i < NV; i++) {
		const float *v = in + i * 4u;
		float t[4], e;
		uint32_t j;

		for (j = 0; j < 4u; j++)
			t[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
		e = 1.0f / t[3];
		for (j = 0; j < 4u; j++)
			out[i * 4u + j] = t[j] * e;
	}
#endif
	for (i = 0; i < NV * 4u; i += 3u)
		sum += out[i];
	return sum;
}

BENCH_KDEF(vxform) = {
	.name = "vxform", .desc = "qemu stub: AltiVec float", .unit = "vert",
	.category = BENCH_CAT_VEC, .flags = BENCH_F_APPROX, .tolerance = 1e-3,
	.units = NV, .arena_bytes = MAT_OFF + 64u,
	.init = vxform_init, .run = vxform_run,
};
