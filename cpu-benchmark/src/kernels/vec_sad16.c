/*
 * vec_sad16.c - VEC: plain-C motion estimation (full search, 16x16 SAD).
 *
 * A video encoder's integer-pel motion search: for 32 16x16 macroblocks of
 * the current 128x64 u8 frame, compute the sum of absolute differences
 * against the reference frame at all 49 positions of a +-3 pixel window and
 * keep the best one (strict <, so the first candidate in raster order wins
 * ties).  No early termination, so the work is exactly 32 * 49 * 256 =
 * 401408 absolute differences.  The SAD is the textbook (x264 C fallback)
 * sad += abs(cur[x] - ref[x]) double loop: a reduction GCC's vectoriser
 * recognises (SAD_EXPR, or ABS_EXPR + WIDEN_SUM_EXPR; psadbw on x86; on
 * AltiVec it needs a sum-across such as vsum4ubs/vsum4shs).
 *
 * Toolchain note: Xenon's VMX has no vsum4* or vmsum* instructions, so
 * testxenon3 keeps this loop scalar (lbzu, subf, srawi/xor/subf, add).
 * test16old at -O2 vectorises it as a WIDEN_SUM_EXPR (V8HI abs -> V4SI),
 * whose widen_ssum expander emits vmsumshm, a pattern its gcc patch
 * disabled ("TARGET_ALTIVEC && 0"): the instruction is silently dropped,
 * the generated loop adds zero vectors and never loads the frames, so every
 * SAD is 0, every block picks (-3, -3) and run() returns 0.  The console
 * then reports WRONG B (B differs from A and from the reference result):
 * that is a test16old bug, not a kernel bug (a plain int16 -> int32 or
 * u8 -> u32 sum loop is miscompiled the same way).  The failure is pinned
 * in tools/qemu/known-failures.txt.
 *
 * The 128x64 frame cannot hold 32 macroblocks on the 16-pixel grid plus a
 * 3-pixel margin, so the blocks sit on a 15x14 grid at (3 + 15 i, 3 + 14 j),
 * i = 0..7, j = 0..3 (they overlap by a pixel or two, as in overlapped
 * motion estimation); every candidate stays inside the reference frame
 * (x in [0, 126], y in [0, 63]).  Rows start at arbitrary byte offsets.
 *
 * Data: the current frame is a textured gradient plus noise; the reference
 * is the current frame moved by a per-region motion vector (8 regions of
 * 32x32, vectors within +-3) plus +-4 of noise, clamped to [0, 255].
 *
 * Result: EXACT, a fold of every block's best SAD and best (dx, dy).
 */
#include <stdlib.h> /* abs() */

#include "bench_kernel.h"

#define FW 128
#define FH 64
#define FRAME_BYTES ((uint32_t)(FW * FH)) /* 8 KB */
#define OFF_CUR 0u
#define OFF_REF FRAME_BYTES
#define ARENA_BYTES (2u * FRAME_BYTES)
#define MB 16
#define RANGE 3
#define NMB 32u

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void sad16_init(void *arena)
{
	/* Motion of each 32x32 region of the reference, (dx, dy). */
	static const signed char mv[2][4][2] = {
		{ { 2, -1 }, { -3, 0 }, { 1, 2 }, { 0, 0 } },
		{ { -2, -2 }, { 3, 1 }, { -1, 3 }, { 2, 2 } },
	};
	uint8_t *cur = bench_at(arena, OFF_CUR);
	uint8_t *ref = bench_at(arena, OFF_REF);
	bench_rng r;
	int x, y;

	bench_rng_seed(&r, 0x73616431u); /* "sad1" */
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++) {
			const int v = ((x * 5 + y * 3) & 127) + ((x ^ y) & 31) +
				      (int)bench_rng_below(&r, 64) + 16;

			cur[y * FW + x] = (uint8_t)v; /* <= 127 + 31 + 63 + 16 */
		}
	for (y = 0; y < FH; y++)
		for (x = 0; x < FW; x++) {
			const signed char *m = mv[y / 32][x / 32];
			const int sx = clampi(x + m[0], 0, FW - 1);
			const int sy = clampi(y + m[1], 0, FH - 1);
			const int v = cur[sy * FW + sx] + (int)bench_rng_below(&r, 9) - 4;

			ref[y * FW + x] = (uint8_t)clampi(v, 0, 255);
		}
}

static uint32_t sad_16x16(const uint8_t *a, const uint8_t *b, int stride)
{
	uint32_t sad = 0;
	int x, y;

	for (y = 0; y < MB; y++) {
		for (x = 0; x < MB; x++)
			sad += (uint32_t)abs(a[x] - b[x]);
		a += stride;
		b += stride;
	}
	return sad;
}

static double sad16_run(void *arena)
{
	const uint8_t *cur = bench_at(arena, OFF_CUR);
	const uint8_t *ref = bench_at(arena, OFF_REF);
	uint32_t h = 0, mb;

	for (mb = 0; mb < NMB; mb++) {
		const int ox = 3 + 15 * (int)(mb & 7u), oy = 3 + 14 * (int)(mb >> 3);
		const uint8_t *c = cur + oy * FW + ox;
		const uint8_t *rc = ref + oy * FW + ox;
		uint32_t best = 0xFFFFFFFFu;
		int bx = 0, by = 0, dx, dy;

		for (dy = -RANGE; dy <= RANGE; dy++)
			for (dx = -RANGE; dx <= RANGE; dx++) {
				const uint32_t s = sad_16x16(c, rc + dy * FW + dx, FW);

				if (s < best) {
					best = s;
					bx = dx;
					by = dy;
				}
			}
		h = bench_mix(h, best);
		h = bench_mix(h, (uint32_t)((by + RANGE) * 8 + (bx + RANGE)));
	}
	return bench_result_u32(h);
}

BENCH_KDEF(sad16) = {
	.name = "sad16", .desc = "16x16 SAD motion search, +-3 px", .unit = "px",
	.category = BENCH_CAT_VEC, .flags = BENCH_F_EXACT, .tolerance = 0.0,
	.units = NMB * (2 * RANGE + 1) * (2 * RANGE + 1) * MB * MB,
	.arena_bytes = ARENA_BYTES,
	.init = sad16_init, .run = sad16_run,
};
