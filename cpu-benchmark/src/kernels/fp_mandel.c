/*
 * fp_mandel.c - FP: float Mandelbrot set, 128x96, max 64 iterations.
 *
 * The escape-time renderer every demo and homebrew sample has: per pixel,
 * c = view origin + pixel index * pitch (an int -> float conversion per
 * pixel), iterate z = z^2 + c in single precision until |z|^2 > 4 or 64
 * iterations, and write the iteration count to an 8-bit image.  The inner
 * loop is a short dependent chain of fmuls / fadds plus an FP compare and an
 * early exit, so it measures FP latency, compare-and-branch cost and how well
 * the compiler schedules a loop with an exit in the middle.  The view (origin
 * and pitch) is read from the arena like a viewer's parameters, so nothing
 * about the image is a compile-time constant.
 *
 * Like most real renderers it first tests c against the main cardioid and
 * the period-2 bulb in closed form (two FP compares) and skips the iteration
 * for those points, which never escape.  Without that test 71% of the
 * iterations would be those 3250 pixels spinning for 64 iterations each;
 * with it a run iterates 84328 times (instead of 292328) and fits the size
 * target.  The image and the result are the same either way.
 *
 * View: [-2.0, 0.6) x [-1.0, 1.0), sampled at pixel centres.  Centres keep
 * every row off the real axis: on it, the set's zero-width antenna
 * [-2, -1.4] would turn on or off with the last bit of cy.
 *
 * Result: total iterations over all pixels (cardioid and bulb pixels count
 * 64), 292328.  APPROX, relative tolerance 1e-3.  Inside the set and far from
 * it, rounding does not change a pixel's count.  The 417 pixels that escape
 * after 20..63 iterations sit near the boundary, where the iteration is
 * chaotic: a 1-ulp difference in c or in one step (fmadds contraction of
 * 2*x*y + cy, x*x - y*y + cx or origin + index * pitch) can grow enough to
 * move the escape iteration.  Measured on the host, contracting everything
 * possible versus nothing changes 2 pixels by 4 iterations in total
 * (1.4e-5); both Xenon compilers emit the same inner-loop FP sequence.  1e-3
 * (292 iterations) is 70x that and still catches a wrong formula, view or
 * limit, which moves the total by a percent or more.
 */
#include "bench_kernel.h"

#define W        128
#define H        96
#define MAX_ITER 64

typedef struct mandel_view {
	float x0, y0; /* c at the centre of pixel (0, 0) */
	float dx, dy; /* pixel pitch */
} mandel_view;

#define OFF_VIEW 0u
#define OFF_IMG  BENCH_LINE
#define ARENA_BYTES (OFF_IMG + (uint32_t)(W * H))

static void mandel_init(void *arena)
{
	mandel_view *v = bench_at(arena, OFF_VIEW);

	v->dx = 2.6f / W;
	v->dy = 2.0f / H;
	v->x0 = -2.0f + 0.5f * v->dx;
	v->y0 = -1.0f + 0.5f * v->dy;
}

/* Main cardioid or period-2 bulb: inside the set, no need to iterate. */
static inline int in_main_bulbs(float cx, float cy)
{
	const float xq = cx - 0.25f, y2 = cy * cy;
	const float q = xq * xq + y2;

	return q * (q + xq) <= 0.25f * y2 ||
	       (cx + 1.0f) * (cx + 1.0f) + y2 <= 0.0625f;
}

static double mandel_run(void *arena)
{
	const mandel_view *v = bench_at(arena, OFF_VIEW);
	uint8_t *img = bench_at(arena, OFF_IMG);
	const float x0 = v->x0, y0 = v->y0, dx = v->dx, dy = v->dy;
	uint32_t total = 0;
	int px, py;

	for (py = 0; py < H; py++) {
		const float cy = y0 + (float)py * dy;
		uint8_t *row = img + py * W;

		for (px = 0; px < W; px++) {
			const float cx = x0 + (float)px * dx;
			int n = MAX_ITER;

			if (!in_main_bulbs(cx, cy)) {
				float x = 0.0f, y = 0.0f;

				for (n = 0; n < MAX_ITER; n++) {
					const float x2 = x * x, y2 = y * y;

					if (x2 + y2 > 4.0f)
						break;
					y = 2.0f * x * y + cy;
					x = x2 - y2 + cx;
				}
			}
			row[px] = (uint8_t)n;
			total += (uint32_t)n;
		}
	}
	return (double)total;
}

BENCH_KDEF(mandel) = {
	.name = "mandel", .desc = "float Mandelbrot 128x96, 64 it", .unit = "px",
	.category = BENCH_CAT_FP, .flags = BENCH_F_APPROX, .tolerance = 1e-3,
	.units = W * H, .arena_bytes = ARENA_BYTES,
	.init = mandel_init, .run = mandel_run,
};
