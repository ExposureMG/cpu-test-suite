/* qemu stub for mathf: every float BLIB() routine, plus the memory and
 * string routines and qsort with a comparator in this file.  APPROX. */
#include "bench_kernel.h"
#include "bench_lib.h"

#define N 1024u
#define S_OFF 8192u
#define Q_OFF 16384u

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

static void mathf_init(void *arena)
{
	float *x = (float *)arena;
	char *s = (char *)bench_at(arena, S_OFF);
	bench_rng r;
	uint32_t i;

	bench_rng_seed(&r, 0xf10a7u);
	for (i = 0; i < N; i++)
		x[i] = bench_rng_f01(&r) * 6.0f - 3.0f;
	for (i = 0; i < 4095u; i++)
		s[i] = (char)('a' + bench_rng_below(&r, 4));
	s[4095] = 0;
	for (i = 0; i < 1024u; i++)
		((uint32_t *)bench_at(arena, Q_OFF))[i] = bench_rng_next(&r);
}

static double mathf_run(void *arena)
{
	const float *x = (const float *)arena;
	const char *s = (const char *)bench_at(arena, S_OFF);
	uint32_t *q = (uint32_t *)bench_at(arena, Q_OFF + 4096u);
	unsigned char *t = (unsigned char *)bench_at(arena, Q_OFF + 8192u);
	float acc = 0.0f;
	double sum;
	uint32_t i, h = 0;

	for (i = 0; i < N; i++) {
		float v = x[i];

		acc += BLIB(sinf)(v) + BLIB(cosf)(v) + BLIB(expf)(v * 0.5f) +
		       BLIB(logf)(v * v + 1.0f) + BLIB(sqrtf)(v * v + 0.25f);
	}
	/* libc routines: lengths, comparisons, a sort */
	h = bench_mix(h, (uint32_t)BLIB(strlen)(s));
	h = bench_mix(h, (uint32_t)(BLIB(strchr)(s, 'd') - s));
	h = bench_mix(h, (uint32_t)(BLIB(strcmp)(s, s + 1) > 0));
	BLIB(memcpy)(q, bench_at(arena, Q_OFF), 4096u);
	BLIB(qsort)(q, 1024u, 4u, cmp_u32);
	for (i = 0; i < 1024u; i += 7u)
		h = bench_mix(h, q[i]);
	BLIB(memset)(t, 0x3c, 1000u);
	BLIB(memmove)(t + 3, t, 500u);
	h = bench_mix(h, (uint32_t)BLIB(memcmp)(t, t + 3, 600u) + 77u);
	sum = (double)acc + (double)(h & 0xffffu);
	return sum;
}

BENCH_KDEF(mathf) = {
	.name = "mathf", .desc = "qemu stub: BLIB float + libc", .unit = "call",
	.category = BENCH_CAT_LIB, .flags = BENCH_F_APPROX, .tolerance = 1e-4,
	.units = 5u * N + 8u, .arena_bytes = Q_OFF + 8192u + 1024u,
	.init = mathf_init, .run = mathf_run,
};
