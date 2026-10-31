/*
 * fp_nbody.c - FP: gravitational N-body integration, 64 bodies, double.
 *
 * The shape of the classic n-body benchmark: every step visits the n(n-1)/2
 * pairs once, computes d = pj - pi, r2 = d.d + eps^2 (Plummer softening) and
 * 1/r^3 = 1 / (r2 * sqrt(r2)) (one fsqrt and one fdiv per pair), kicks both
 * velocities, and then drifts all positions (symplectic Euler).  Body i's
 * position and velocity stay in registers while its pairs are visited, as in
 * hand-tuned game physics code.  run() resets the state from the input copy
 * (bench_copy of 4 KB), integrates 4 steps and returns the total energy.
 *
 * Work: 4 steps x 2016 pairs = 8064 pairs (the unit).  The energy evaluation
 * at the end visits the 2016 pairs once more (sqrt + divide each) as the
 * result fold, so one run() does 10080 pair evaluations in total.
 *
 * Data: positions in [-10, 10)^3, velocities in [-0.1, 0.1)^3, masses in
 * [0.5, 1.5), G = 1, dt = 0.01, eps^2 = 0.01.  The closest pair of this set
 * is 1.66 apart and the largest acceleration is 0.63, so the run is far from
 * the softening scale and smooth: a perturbation grows by a factor of about
 * 1 + dt * sqrt(m / r_min^3) = 1.006 per step.
 *
 * Result: total energy E = sum m v^2 / 2 - sum m_i m_j / sqrt(r2) = -165.17
 * (kinetic 0.31, potential -165.48).  APPROX, relative tolerance 1e-9.
 * Compilers may contract d.d, the kicks and the drifts into fmadd
 * differently: at most ~3 ulp (3e-16 relative) per pair term or state value
 * per step, essentially not amplified over 4 steps.  Worst case, all 2080
 * energy terms off by 3 ulp in the same direction (1.6e-13) plus a different
 * rounding of the accumulator at every addition (2080 x ulp(165) = 6e-11),
 * is 4e-13 relative; the expected value is a few ulp of E (1e-15).  Measured
 * on the host: with and without fmadd contraction E is bit-identical.  1e-9
 * still catches any real bug: the smallest single pair term is 4e-5 of E.
 */
#include "bench_kernel.h"

#define NBODY  64
#define NSTEPS 4
#define DT     0.01
#define EPS2   0.01 /* softening length squared */

typedef struct body {
	double x, y, z;    /* position */
	double vx, vy, vz; /* velocity */
	double mass;
	double pad;        /* 64-byte stride: two bodies per cache line */
} body;

#define STATE_BYTES ((uint32_t)(NBODY * sizeof(body))) /* 4 KB */
#define OFF_INPUT 0u
#define OFF_STATE BENCH_ALIGN_UP(OFF_INPUT + STATE_BYTES, BENCH_LINE)
#define ARENA_BYTES (OFF_STATE + STATE_BYTES)

/* Uniform in [-scale, scale): 2 * u - 1 is exact, so one rounding. */
static double rand_sym(bench_rng *r, double scale)
{
	return scale * (2.0 * bench_rng_d01(r) - 1.0);
}

static void nbody_init(void *arena)
{
	body *b = bench_at(arena, OFF_INPUT);
	bench_rng r;
	int i;

	bench_rng_seed(&r, 0x6E626479u); /* "nbdy" */
	for (i = 0; i < NBODY; i++) {
		b[i].x = rand_sym(&r, 10.0);
		b[i].y = rand_sym(&r, 10.0);
		b[i].z = rand_sym(&r, 10.0);
		b[i].vx = rand_sym(&r, 0.1);
		b[i].vy = rand_sym(&r, 0.1);
		b[i].vz = rand_sym(&r, 0.1);
		b[i].mass = 0.5 + bench_rng_d01(&r);
		b[i].pad = 0.0;
	}
}

/* One symplectic Euler step: kick all pairs, then drift. */
static void advance(body *b, int n, double dt)
{
	int i, j;

	for (i = 0; i < n; i++) {
		body *bi = &b[i];
		const double xi = bi->x, yi = bi->y, zi = bi->z;
		const double mi_dt = bi->mass * dt;
		double vxi = bi->vx, vyi = bi->vy, vzi = bi->vz;

		for (j = i + 1; j < n; j++) {
			body *bj = &b[j];
			const double dx = bj->x - xi;
			const double dy = bj->y - yi;
			const double dz = bj->z - zi;
			const double r2 = dx * dx + dy * dy + dz * dz + EPS2;
			const double inv_r3 = 1.0 / (r2 * __builtin_sqrt(r2));
			const double si = bj->mass * dt * inv_r3;
			const double sj = mi_dt * inv_r3;

			vxi += dx * si; /* i is pulled towards j ... */
			vyi += dy * si;
			vzi += dz * si;
			bj->vx -= dx * sj; /* ... and j towards i */
			bj->vy -= dy * sj;
			bj->vz -= dz * sj;
		}
		bi->vx = vxi;
		bi->vy = vyi;
		bi->vz = vzi;
	}
	for (i = 0; i < n; i++) {
		b[i].x += dt * b[i].vx;
		b[i].y += dt * b[i].vy;
		b[i].z += dt * b[i].vz;
	}
}

/* Kinetic plus (softened) potential energy. */
static double energy(const body *b, int n)
{
	double e = 0.0;
	int i, j;

	for (i = 0; i < n; i++) {
		const body *bi = &b[i];

		e += 0.5 * bi->mass *
		     (bi->vx * bi->vx + bi->vy * bi->vy + bi->vz * bi->vz);
		for (j = i + 1; j < n; j++) {
			const body *bj = &b[j];
			const double dx = bj->x - bi->x;
			const double dy = bj->y - bi->y;
			const double dz = bj->z - bi->z;

			e -= bi->mass * bj->mass /
			     __builtin_sqrt(dx * dx + dy * dy + dz * dz + EPS2);
		}
	}
	return e;
}

static double nbody_run(void *arena)
{
	body *b = bench_at(arena, OFF_STATE);
	int s;

	bench_copy(b, bench_at(arena, OFF_INPUT), STATE_BYTES);
	for (s = 0; s < NSTEPS; s++)
		advance(b, NBODY, DT);
	return energy(b, NBODY);
}

BENCH_KDEF(nbody) = {
	.name = "nbody", .desc = "64 bodies, 4 steps, double", .unit = "pair",
	.category = BENCH_CAT_FP, .flags = BENCH_F_APPROX, .tolerance = 1e-9,
	.units = NSTEPS * NBODY * (NBODY - 1) / 2, .arena_bytes = ARENA_BYTES,
	.init = nbody_init, .run = nbody_run,
};
