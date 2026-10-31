/*
 * vec_vxform.c - VEC: AltiVec vertex transform with perspective divide.
 *
 * 8192 vertices (x, y, z, 1) stored as AoS vector floats are multiplied by a
 * 4x4 view-projection matrix (column vectors, clip = M * v) and projected:
 * x, y and z are multiplied by 1/w and the output vertex is
 * (x/w, y/w, z/w, 1/w), the screen-space form a rasteriser wants (1/w kept
 * for perspective-correct interpolation).
 *
 * The loop works on 4 vertices at a time, SoA style: the 4 AoS vertices
 * are transposed with vec_mergeh/vec_mergel, the 16 matrix elements are
 * splatted once before the loop and multiplied in with vec_madd chains
 * (4 independent chains of 4 multiply-adds: x', y', z', w'), 1/w comes from
 * vec_re refined by one Newton-Raphson step (e' = e + e * (1 - w * e) with
 * vec_nmsub / vec_madd), and the result is transposed back and stored.
 * The 16 loop-invariant splats, -0.0, 1.0 and the 4 accumulators (22
 * registers) plus the per-iteration temporaries need about 32 live vector
 * registers, the whole AltiVec register file, so the 128 registers of
 * VMX128 can matter (at -O2 test16old just fits in v0-v31 without spilling;
 * testxenon3 uses v32-v55).
 *
 * AltiVec has no plain float multiply (vec_mul on vector float needs VSX):
 * every product is a vec_madd, with a -0.0 addend where there is nothing to
 * add (a * b + -0.0 == a * b exactly, including the sign of zero).
 *
 * Data: x, y in [-4, 4), z in [2, 12), w = 1.  M = P * V with V a rotation
 * about y and x by atan(7/24) (cos 0.96, sin 0.28) plus a translation
 * (0.5, -0.25, 3) and P a D3D-style perspective projection (xs 1.2, ys 1.6,
 * zn 0.5, zf 100).  w' = -0.2688x + 0.28y + 0.9216z + 3 lies in
 * [2.65, 16.3]: no division by ~0, no denormals, no inf/NaN anywhere.
 *
 * Host build (no <altivec.h> on x86): the #else branch is a scalar version
 * with the same data layout and algorithm (4 vertices per step, the same 4
 * accumulators); it uses 1/w instead of the vec_re hardware estimate, so its
 * result differs from the AltiVec one in the last bits.  It only checks
 * memory safety and idempotency.
 *
 * Result: the sum of all outputs (x/w + y/w + z/w + 1/w over 8192 vertices,
 * about 8.4e3 with this data, dominated by z/w ~ 0.9 and 1/w ~ 0.1: never
 * near zero).  Accumulated per lane in 4 float vector accumulators, reduced
 * at the end in double.  APPROX, relative tolerance 1e-6.  Every FP
 * operation is an explicit intrinsic with one rounding (vmaddfp/vnmsubfp are
 * fused, vrefp is the same hardware estimate for both variants), so A and B
 * are expected to be bit-identical (they are under qemu-ppc).  What a
 * compiler may legitimately change is the rounding of an individual
 * operation (e.g. a -0.0-addend vmaddfp turned into vmulfp128, a
 * fused/unfused choice): about 1 ulp (6e-8 relative) per output component.
 * The direct effect on the sum is <= 8192 * 4 * 6e-8 * |o| ~ 1e-3; the
 * accumulators (2048 float adds per lane, ulp <= 1.2e-4 near 1.9e3) only
 * diverge when such a difference crosses a rounding boundary (probability
 * ~ 6e-8 / 1.2e-4 per add), a few ulps of the accumulator in total.
 * Simulated with this data and +-1 ulp on every output component: <= 1.0e-7
 * relative over 20 trials.  1e-6 is 10x that; a real bug (a wrong matrix
 * element, a swapped lane, a missing vertex) moves the sum by >= ~0.1
 * (>= 1e-5 relative) and fails the check.
 */
#include "bench_kernel.h"

#ifdef __ALTIVEC__
#include <altivec.h>
#endif

#define NVERT 8192u

#define MAT_BYTES 128u                   /* 4x4 floats, padded to a line */
#define VERT_BYTES (NVERT * 16u)         /* 128 KB */
#define OFF_MAT 0u
#define OFF_IN (OFF_MAT + MAT_BYTES)
#define OFF_OUT (OFF_IN + VERT_BYTES)
#define ARENA_BYTES (OFF_OUT + VERT_BYTES)

/* Element (row, col) of the column-major matrix m. */
#define M_AT(m, row, col) ((m)[4 * (col) + (row)])

static void vxform_init(void *arena)
{
	float *m = bench_at(arena, OFF_MAT);
	float *v = bench_at(arena, OFF_IN);
	const float c = 0.96f, s = 0.28f;    /* cos, sin of atan(7/24) */
	const float xs = 1.2f, ys = 1.6f, zn = 0.5f, zf = 100.0f;
	const float q = zf / (zf - zn);
	const float tx = 0.5f, ty = -0.25f, tz = 3.0f;
	bench_rng rng;
	uint32_t i, j;

	/* M = P * [R | t] with R = Rx * Ry:
	 *   R = | c      0   s     |
	 *       | s*s    c   -s*c  |
	 *       | -c*s   s   c*c   |
	 * P scales row 0 by xs and row 1 by ys, maps row 2 to q * (z - zn)
	 * and copies the view z into w (row 3). */
	for (j = 0; j < 4; j++) {
		const float r0 = j == 0 ? c : j == 1 ? 0.0f : j == 2 ? s : tx;
		const float r1 = j == 0 ? s * s : j == 1 ? c : j == 2 ? -s * c : ty;
		const float r2 = j == 0 ? -c * s : j == 1 ? s : j == 2 ? c * c : tz;

		M_AT(m, 0, j) = xs * r0;
		M_AT(m, 1, j) = ys * r1;
		M_AT(m, 2, j) = j == 3 ? q * r2 - q * zn : q * r2;
		M_AT(m, 3, j) = r2;
	}
	bench_fill(m + 16, 0u, MAT_BYTES - 16u * 4u); /* padding: +0.0f bits */

	bench_rng_seed(&rng, 0x76786672u); /* "vxfr" */
	for (i = 0; i < NVERT; i++) {
		v[4 * i + 0] = 8.0f * bench_rng_f01(&rng) - 4.0f;
		v[4 * i + 1] = 8.0f * bench_rng_f01(&rng) - 4.0f;
		v[4 * i + 2] = 10.0f * bench_rng_f01(&rng) + 2.0f;
		v[4 * i + 3] = 1.0f;
	}
}

#ifdef __ALTIVEC__

static double vxform_run(void *arena)
{
	const float *m = bench_at(arena, OFF_MAT);
	const float *in = bench_at(arena, OFF_IN);
	float *out = bench_at(arena, OFF_OUT);
	const vector float vnz = { -0.0f, -0.0f, -0.0f, -0.0f };
	const vector float vone = { 1.0f, 1.0f, 1.0f, 1.0f };
	const vector float c0 = vec_ld(0, m), c1 = vec_ld(16, m);
	const vector float c2 = vec_ld(32, m), c3 = vec_ld(48, m);
	/* mRC = element R of column C, splatted once. */
	const vector float m00 = vec_splat(c0, 0), m10 = vec_splat(c0, 1);
	const vector float m20 = vec_splat(c0, 2), m30 = vec_splat(c0, 3);
	const vector float m01 = vec_splat(c1, 0), m11 = vec_splat(c1, 1);
	const vector float m21 = vec_splat(c1, 2), m31 = vec_splat(c1, 3);
	const vector float m02 = vec_splat(c2, 0), m12 = vec_splat(c2, 1);
	const vector float m22 = vec_splat(c2, 2), m32 = vec_splat(c2, 3);
	const vector float m03 = vec_splat(c3, 0), m13 = vec_splat(c3, 1);
	const vector float m23 = vec_splat(c3, 2), m33 = vec_splat(c3, 3);
	vector float acc0 = { 0.0f, 0.0f, 0.0f, 0.0f };
	vector float acc1 = acc0, acc2 = acc0, acc3 = acc0;
	union {
		vector float v;
		float f[4];
	} sum;
	uint32_t i;

	for (i = 0; i < NVERT * 4u; i += 16) {
		const vector float v0 = vec_ld(0, in + i), v1 = vec_ld(16, in + i);
		const vector float v2 = vec_ld(32, in + i), v3 = vec_ld(48, in + i);
		/* AoS -> SoA */
		const vector float t0 = vec_mergeh(v0, v2); /* x0 x2 y0 y2 */
		const vector float t1 = vec_mergeh(v1, v3); /* x1 x3 y1 y3 */
		const vector float t2 = vec_mergel(v0, v2); /* z0 z2 w0 w2 */
		const vector float t3 = vec_mergel(v1, v3); /* z1 z3 w1 w3 */
		const vector float x = vec_mergeh(t0, t1);  /* x0 x1 x2 x3 */
		const vector float y = vec_mergel(t0, t1);
		const vector float z = vec_mergeh(t2, t3);
		const vector float w = vec_mergel(t2, t3);
		/* clip = M * v: four independent multiply-add chains */
		const vector float cx = vec_madd(m00, x, vec_madd(m01, y,
				vec_madd(m02, z, vec_madd(m03, w, vnz))));
		const vector float cy = vec_madd(m10, x, vec_madd(m11, y,
				vec_madd(m12, z, vec_madd(m13, w, vnz))));
		const vector float cz = vec_madd(m20, x, vec_madd(m21, y,
				vec_madd(m22, z, vec_madd(m23, w, vnz))));
		const vector float cw = vec_madd(m30, x, vec_madd(m31, y,
				vec_madd(m32, z, vec_madd(m33, w, vnz))));
		/* 1/w: estimate + one Newton-Raphson step */
		const vector float e = vec_re(cw);
		const vector float rw = vec_madd(e, vec_nmsub(e, cw, vone), e);
		const vector float px = vec_madd(cx, rw, vnz);
		const vector float py = vec_madd(cy, rw, vnz);
		const vector float pz = vec_madd(cz, rw, vnz);
		/* SoA -> AoS: (x/w, y/w, z/w, 1/w) */
		const vector float u0 = vec_mergeh(px, pz); /* px0 pz0 px1 pz1 */
		const vector float u1 = vec_mergeh(py, rw); /* py0 rw0 py1 rw1 */
		const vector float u2 = vec_mergel(px, pz);
		const vector float u3 = vec_mergel(py, rw);
		const vector float o0 = vec_mergeh(u0, u1); /* px0 py0 pz0 rw0 */
		const vector float o1 = vec_mergel(u0, u1);
		const vector float o2 = vec_mergeh(u2, u3);
		const vector float o3 = vec_mergel(u2, u3);

		vec_st(o0, 0, out + i);
		vec_st(o1, 16, out + i);
		vec_st(o2, 32, out + i);
		vec_st(o3, 48, out + i);
		acc0 = vec_add(acc0, o0);
		acc1 = vec_add(acc1, o1);
		acc2 = vec_add(acc2, o2);
		acc3 = vec_add(acc3, o3);
	}
	sum.v = vec_add(vec_add(acc0, acc1), vec_add(acc2, acc3));
	return ((double)sum.f[0] + (double)sum.f[1]) +
	       ((double)sum.f[2] + (double)sum.f[3]);
}

#else /* !__ALTIVEC__: scalar version for the host build */

static double vxform_run(void *arena)
{
	const float *m = bench_at(arena, OFF_MAT);
	const float *in = bench_at(arena, OFF_IN);
	float *out = bench_at(arena, OFF_OUT);
	float acc[4][4];
	float sum[4];
	uint32_t i, k, j;

	for (k = 0; k < 4; k++)
		for (j = 0; j < 4; j++)
			acc[k][j] = 0.0f;
	for (i = 0; i < NVERT * 4u; i += 16) {
		for (k = 0; k < 4; k++) {
			const float *p = in + i + 4 * k;
			float *o = out + i + 4 * k;
			const float x = p[0], y = p[1], z = p[2], w = p[3];
			const float cx = m[0] * x + (m[4] * y + (m[8] * z + m[12] * w));
			const float cy = m[1] * x + (m[5] * y + (m[9] * z + m[13] * w));
			const float cz = m[2] * x + (m[6] * y + (m[10] * z + m[14] * w));
			const float cw = m[3] * x + (m[7] * y + (m[11] * z + m[15] * w));
			const float e = 1.0f / cw;
			const float rw = e + e * (1.0f - e * cw);

			o[0] = cx * rw;
			o[1] = cy * rw;
			o[2] = cz * rw;
			o[3] = rw;
			for (j = 0; j < 4; j++)
				acc[k][j] += o[j];
		}
	}
	for (j = 0; j < 4; j++)
		sum[j] = (acc[0][j] + acc[1][j]) + (acc[2][j] + acc[3][j]);
	return ((double)sum[0] + (double)sum[1]) + ((double)sum[2] + (double)sum[3]);
}

#endif /* __ALTIVEC__ */

BENCH_KDEF(vxform) = {
	.name = "vxform", .desc = "vec4 x mat4 + vec_re persp div", .unit = "vert",
	.category = BENCH_CAT_VEC, .flags = BENCH_F_APPROX, .tolerance = 1e-6,
	.units = NVERT, .arena_bytes = ARENA_BYTES,
	.init = vxform_init, .run = vxform_run,
};
