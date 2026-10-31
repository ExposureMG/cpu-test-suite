/*
 * mix_raytrace.c - MIX: small single-precision ray tracer, 64x48.
 *
 * The "spheres on a checkerboard" tracer of countless demos and homebrew
 * samples, all in float: one primary ray per pixel through a pinhole camera
 * (pixel index -> float, direction normalised with 1.0f / sqrtf()), nearest
 * hit over a ground plane y = -1 and 6 spheres (the usual quadratic with the
 * "outside and pointing away" early out, sqrtf only for real roots; sphere
 * normals use the precomputed 1 / radius), a hard shadow ray toward one
 * directional light (first occluder wins), Lambert + ambient shading, and
 * one mirror bounce for the reflective spheres (the bounce is shaded, with
 * its own shadow ray, but not reflected again).  Misses see a sky gradient;
 * the ground is a checkerboard picked by float -> int conversions of the
 * hit point.  Each channel is clamped to [0, 1] and converted to an RGB8
 * pixel.  Of the 3072 primary rays 47% see the sky, 26% the ground, 27% a
 * sphere, and 20% take the mirror bounce.
 *
 * It measures dependent fmadd/fmsub chains, FP compares feeding branches
 * (fcmpu), fsqrts and fdivs, int <-> float conversions and what each
 * compiler makes of small vec3-by-value helpers (inlined, or passed through
 * the stack).  The scene (camera, light, spheres, colours) is read from the
 * arena, so nothing is a compile-time constant.
 *
 * Size: DESIGN.md asks for 96x64; that took 440 us per run on the host,
 * well over rule 9's ~300 us (and FP-latency-bound code like this loses
 * more than average on the in-order Xenon), so the image is 64x48: 3072
 * rays, 200 us per run on the host.
 *
 * Result: the sum of all 9216 output bytes, as a double.  APPROX, relative
 * tolerance 1e-3.  Both compilers contract a*b+c into fmadds where they
 * like (-ffp-contract=fast is the GNU C default), so a value can differ in
 * its last bits between them.  That moves a byte only when a channel lies
 * within an ulp of a rounding boundary (+-1) or a ray grazes an edge
 * (silhouette, shadow or checker boundary; one byte can then change by up
 * to 255).  Measured: host x86 with all FMA contraction (46 vfmadd/vfmsub)
 * versus none, -O0 and -O3 all give the same 9216 bytes (sum 1399500), as
 * do both Xenon compilers under qemu-ppc; moving every float of the scene
 * by up to +-2 ulp (200 random trials) changes the sum by at most 3.2e-5
 * (45).  1e-3 (1400) is 30x that, while real defects move the sum much
 * further: no shadow ray on the bounce +0.37%, no shadow rays +2.5%, a
 * wrong channel -4.7%, an inverted sphere normal -12.6%.
 *
 * Arena layout (offsets are multiples of BENCH_LINE):
 *   OFF_SCENE  rt_scene                332 B  (read-only in run)
 *   OFF_IMG    uint8_t rgb[48][64][3]  9 KB   (written by run)
 */
#include <math.h> /* sqrtf: inline fsqrts (GCC may add its errno fallback call) */

#include "bench_kernel.h"

#define RT_W       64
#define RT_H       48
#define RT_SPHERES 6
#define RT_EPS     1.0e-3f /* self-intersection offset / minimum t */
#define RT_FAR     64.0f   /* nothing beyond this distance (and no ground) */

typedef struct rt_vec {
	float x, y, z;
} rt_vec;

typedef struct rt_sphere {
	rt_vec c;    /* centre */
	float r2;    /* radius squared */
	rt_vec rgb;  /* albedo */
	float refl;  /* mirror reflectance, 0 = matte */
	float inv_r; /* 1 / radius, for the normal */
} rt_sphere;

typedef struct rt_scene {
	rt_vec eye;         /* camera position */
	rt_vec corner;      /* direction to the centre of pixel (0, 0) */
	rt_vec du, dv;      /* direction step per pixel in x and in y */
	rt_vec light;       /* unit vector toward the light */
	float ambient;
	float ground_y;     /* the plane y = ground_y */
	rt_vec check0, check1; /* checkerboard albedos */
	rt_vec horizon, zenith; /* sky gradient */
	rt_sphere sph[RT_SPHERES];
} rt_scene;

#define OFF_SCENE   0u
#define OFF_IMG     BENCH_ALIGN_UP((uint32_t)sizeof(rt_scene), BENCH_LINE)
#define ARENA_BYTES (OFF_IMG + RT_W * RT_H * 3u)

#define RT_NONE   (-1)
#define RT_GROUND RT_SPHERES

static inline rt_vec v3(float x, float y, float z)
{
	rt_vec v = { x, y, z };
	return v;
}

static inline rt_vec v_add(rt_vec a, rt_vec b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline rt_vec v_sub(rt_vec a, rt_vec b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline rt_vec v_scale(rt_vec a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v_dot(rt_vec a, rt_vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static inline rt_vec v_norm(rt_vec a)
{
	return v_scale(a, 1.0f / sqrtf(v_dot(a, a)));
}

/* Nearest hit along o + t d (d unit) with RT_EPS < t < RT_FAR.  Returns the
 * object index (RT_GROUND for the plane) or RT_NONE. */
static int rt_nearest(const rt_scene *sc, rt_vec o, rt_vec d, float *t_hit)
{
	float tmin = RT_FAR;
	int hit = RT_NONE, i;

	if (d.y < 0.0f) {
		float t = (sc->ground_y - o.y) / d.y;

		if (t > RT_EPS && t < tmin) {
			tmin = t;
			hit = RT_GROUND;
		}
	}
	for (i = 0; i < RT_SPHERES; i++) {
		const rt_sphere *s = &sc->sph[i];
		rt_vec l = v_sub(s->c, o);
		float b = v_dot(l, d);
		float c = v_dot(l, l) - s->r2;
		float disc, sq, t;

		if (c > 0.0f && b < 0.0f)
			continue; /* outside, and the sphere is behind us */
		disc = b * b - c;
		if (disc < 0.0f)
			continue;
		sq = sqrtf(disc);
		t = b - sq;
		if (t < RT_EPS)
			t = b + sq;
		if (t > RT_EPS && t < tmin) {
			tmin = t;
			hit = i;
		}
	}
	*t_hit = tmin;
	return hit;
}

/* Is anything between o and the (infinitely distant) light along d? */
static int rt_occluded(const rt_scene *sc, rt_vec o, rt_vec d)
{
	int i;

	for (i = 0; i < RT_SPHERES; i++) {
		const rt_sphere *s = &sc->sph[i];
		rt_vec l = v_sub(s->c, o);
		float b = v_dot(l, d);
		float c = v_dot(l, l) - s->r2;
		float disc;

		if (b < 0.0f)
			continue;
		disc = b * b - c;
		if (disc > 0.0f && b - sqrtf(disc) > RT_EPS)
			return 1;
	}
	return 0;
}

/* Hit point, unit normal and albedo of object 'id' at distance t. */
static void rt_surface(const rt_scene *sc, int id, rt_vec o, rt_vec d, float t,
		       rt_vec *p, rt_vec *n, rt_vec *albedo, float *refl)
{
	*p = v_add(o, v_scale(d, t));
	if (id == RT_GROUND) {
		/* |x|, |z| < RT_FAR here, so the conversions are in range and
		 * truncation of the biased (positive) value is floor(). */
		int cx = (int)(p->x + 1024.0f), cz = (int)(p->z + 1024.0f);

		*n = v3(0.0f, 1.0f, 0.0f);
		*albedo = ((cx ^ cz) & 1) ? sc->check1 : sc->check0;
		*refl = 0.0f;
	} else {
		const rt_sphere *s = &sc->sph[id];

		*n = v_scale(v_sub(*p, s->c), s->inv_r);
		*albedo = s->rgb;
		*refl = s->refl;
	}
}

/* Lambert + ambient with a hard shadow. */
static rt_vec rt_light(const rt_scene *sc, rt_vec p, rt_vec n, rt_vec albedo)
{
	float ndl = v_dot(n, sc->light);
	float k = sc->ambient;

	if (ndl > 0.0f && !rt_occluded(sc, v_add(p, v_scale(n, RT_EPS)), sc->light))
		k += (1.0f - sc->ambient) * ndl;
	return v_scale(albedo, k);
}

static rt_vec rt_sky(const rt_scene *sc, rt_vec d)
{
	float t = d.y > 0.0f ? d.y : 0.0f;

	return v_add(v_scale(sc->horizon, 1.0f - t), v_scale(sc->zenith, t));
}

/* Colour seen along o + t d: local shading plus one mirror bounce. */
static rt_vec rt_trace(const rt_scene *sc, rt_vec o, rt_vec d)
{
	rt_vec p, n, albedo, col, bounce;
	float t, refl;
	int id = rt_nearest(sc, o, d, &t);

	if (id == RT_NONE)
		return rt_sky(sc, d);
	rt_surface(sc, id, o, d, t, &p, &n, &albedo, &refl);
	col = rt_light(sc, p, n, albedo);
	if (refl > 0.0f) {
		/* r = d - 2 (d.n) n; the bounce is shaded but not reflected. */
		rt_vec r = v_sub(d, v_scale(n, 2.0f * v_dot(d, n)));
		rt_vec q = v_add(p, v_scale(n, RT_EPS));
		float tb, rb;
		int ib = rt_nearest(sc, q, r, &tb);

		if (ib == RT_NONE) {
			bounce = rt_sky(sc, r);
		} else {
			rt_vec pb, nb, ab;

			rt_surface(sc, ib, q, r, tb, &pb, &nb, &ab, &rb);
			bounce = rt_light(sc, pb, nb, ab);
		}
		col = v_add(v_scale(col, 1.0f - refl), v_scale(bounce, refl));
	}
	return col;
}

static inline uint32_t rt_to_u8(float c)
{
	if (c < 0.0f)
		c = 0.0f;
	if (c > 1.0f)
		c = 1.0f;
	return (uint32_t)(c * 255.0f + 0.5f);
}

static void raytrace_init(void *arena)
{
	static const struct {
		float x, y, z, r, cr, cg, cb, refl;
	} spheres[RT_SPHERES] = {
		{ 0.00f, 0.00f, 1.60f, 1.00f, 0.90f, 0.90f, 0.95f, 0.75f }, /* mirror */
		{ -1.90f, -0.45f, 1.00f, 0.55f, 0.90f, 0.20f, 0.15f, 0.00f },
		{ 1.80f, -0.40f, 0.80f, 0.60f, 0.20f, 0.80f, 0.30f, 0.35f },
		{ -0.75f, -0.75f, 0.15f, 0.25f, 0.20f, 0.35f, 0.95f, 0.00f },
		{ 0.85f, -0.78f, 0.05f, 0.22f, 0.95f, 0.85f, 0.20f, 0.20f },
		{ -0.60f, 0.40f, 5.50f, 1.40f, 0.60f, 0.30f, 0.80f, 0.15f },
	};
	rt_scene *sc = bench_at(arena, OFF_SCENE);
	int i;

	/* 1.5 x 1.125 image plane at distance 1: square pixels, exact
	 * 3/128 steps. */
	sc->eye = v3(0.0f, 0.30f, -2.60f);
	sc->du = v3(1.5f / RT_W, 0.0f, 0.0f);
	sc->dv = v3(0.0f, -1.125f / RT_H, 0.0f);
	sc->corner = v3(-0.75f + 0.5f * sc->du.x, 0.5625f + 0.5f * sc->dv.y, 1.0f);
	sc->light = v_norm(v3(-0.50f, 0.90f, -0.55f));
	sc->ambient = 0.15f;
	sc->ground_y = -1.0f;
	sc->check0 = v3(0.85f, 0.85f, 0.80f);
	sc->check1 = v3(0.25f, 0.25f, 0.30f);
	sc->horizon = v3(0.85f, 0.90f, 1.00f);
	sc->zenith = v3(0.25f, 0.45f, 0.85f);
	for (i = 0; i < RT_SPHERES; i++) {
		sc->sph[i].c = v3(spheres[i].x, spheres[i].y, spheres[i].z);
		sc->sph[i].r2 = spheres[i].r * spheres[i].r;
		sc->sph[i].rgb = v3(spheres[i].cr, spheres[i].cg, spheres[i].cb);
		sc->sph[i].refl = spheres[i].refl;
		sc->sph[i].inv_r = 1.0f / spheres[i].r;
	}
}

static double raytrace_run(void *arena)
{
	const rt_scene *sc = bench_at(arena, OFF_SCENE);
	uint8_t *rgb = bench_at(arena, OFF_IMG);
	uint32_t sum = 0;
	int x, y;

	for (y = 0; y < RT_H; y++) {
		for (x = 0; x < RT_W; x++) {
			rt_vec d = v_add(sc->corner, v_add(v_scale(sc->du, (float)x),
							  v_scale(sc->dv, (float)y)));
			rt_vec c = rt_trace(sc, sc->eye, v_norm(d));
			uint32_t r = rt_to_u8(c.x), g = rt_to_u8(c.y), b = rt_to_u8(c.z);

			rgb[0] = (uint8_t)r;
			rgb[1] = (uint8_t)g;
			rgb[2] = (uint8_t)b;
			rgb += 3;
			sum += r + g + b;
		}
	}
	return (double)sum;
}

BENCH_KDEF(raytrace) = {
	.name = "raytrace", .desc = "float ray tracer 64x48, 6 sph", .unit = "ray",
	.category = BENCH_CAT_MIX, .flags = BENCH_F_APPROX, .tolerance = 1e-3,
	.units = RT_W * RT_H, .arena_bytes = ARENA_BYTES,
	.init = raytrace_init, .run = raytrace_run,
};
