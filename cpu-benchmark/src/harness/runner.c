/*
 * runner.c - the measurement method of DESIGN.md section 1.  Portable C: it
 * talks to the machine only through platform.h.
 *
 * Per kernel i (A = variant 0, B = variant 1):
 *   1. A and B descriptors must agree (name, category, flags, units, ...).
 *      Poison the arena bytes the kernel declares, write guard patterns
 *      before the arena and right after arena_bytes, call A.init() once.
 *   2. Warm-up: A.run() and B.run() once; these are the reference results.
 *   3. Calibrate reps with A so one sample (reps back-to-back run() calls,
 *      timed as a whole) takes about sample_ms.  B uses the same reps.
 *   4. samples per variant, alternating AB, BA, AB, ...  Every call's result
 *      is compared bit-exactly with its variant's reference (else UNSTABLE).
 *   5. min / median / max ticks per run(); time = min.
 *   6. Cross-check A vs B (EXACT: bit-identical, APPROX: relative
 *      tolerance), guards intact (else OVERRUN), and each variant's result
 *      against the kernel's big-endian reference result in refs.h, with the
 *      same rule (else WRONG A / WRONG B).  Only A's init runs, so a
 *      miscompiled init would give both variants the same wrong input and no
 *      MISMATCH: the reference catches it.
 *   7. Geometric means of B/A over the kernels that passed.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "refs.h"
#include "runner.h"

/* Upper bound for reps.  Kernels take 0.1-2 ms on Xenon and 0.01-0.3 ms on
 * a host, so this is never reached by a sane kernel; it bounds the run time
 * if the timer misbehaves or a run() does no work. */
#define REPS_MAX (1u << 18)

static const char *const cat_names[BENCH_CAT_COUNT] = {
	"INT", "BR", "MEM", "FP", "VEC", "MIX", "LIB",
};

const char *bench_cat_name(uint32_t cat)
{
	return cat < BENCH_CAT_COUNT ? cat_names[cat] : "?";
}

const char *bench_status_word(uint32_t s, int nvar)
{
	if (s & BST_MISSING)
		return "MISSING";
	if (s & BST_BADDESC)
		return "BADDESC";
	if (s & BST_NOTIME)
		return "NOTIMER";
	if (s & BST_OVERRUN)
		return "OVERRUN";
	if (s & BST_UNSTABLE)
		return "UNSTABLE";
	if ((s & (BST_WRONG_A | BST_WRONG_B)) == (BST_WRONG_A | BST_WRONG_B))
		return "WRONG AB";
	if (s & BST_WRONG_A)
		return nvar == 2 ? "WRONG A" : "WRONG";
	if (s & BST_WRONG_B)
		return "WRONG B";
	if (s & BST_MISMATCH)
		return "MISMATCH";
	return "ok";
}

const char *bench_ref_word(int ref)
{
	switch (ref) {
	case REF_CHECKED:
		return "checked";
	case REF_STALE:
		return "stale";
	case REF_BYTEORDER:
		return "byteorder";
	default:
		return "none";
	}
}

double bench_us(const bench_results *r, double ticks)
{
	return r->tb_hz ? ticks * 1e6 / (double)r->tb_hz : 0.0;
}

double bench_cyc_per_unit(const bench_results *r, const bench_kernel *k,
			  double ticks)
{
	if (r->cyc_per_tick <= 0.0 || !k || !k->units)
		return 0.0;
	return ticks * r->cyc_per_tick / (double)k->units;
}

double bench_munits_per_s(const bench_results *r, const bench_kernel *k,
			  double ticks)
{
	double us = bench_us(r, ticks);

	if (us <= 0.0 || !k)
		return 0.0;
	return (double)k->units / us;
}

double bench_noise_pct(const bench_vres *v)
{
	if (!(v->tmin > 0.0))
		return 0.0;
	return (v->tmed - v->tmin) / v->tmin * 100.0;
}

/* ---- helpers --------------------------------------------------------- */

typedef double (*run_fn)(void *arena);

typedef union {
	double d;
	uint64_t u;
} dbits;

static uint64_t bits_of(double d)
{
	dbits x;

	x.d = d;
	return x.u;
}

static uint8_t guard_byte(uint32_t i, uint32_t salt)
{
	uint32_t x = (i + salt * 0x51u) * 0x9E3779B1u;

	return (uint8_t)((x >> 24) ^ (x >> 11) ^ 0x5Au);
}

static void guard_write(uint8_t *p, uint32_t salt)
{
	uint32_t i;

	for (i = 0; i < BENCH_GUARD_BYTES; i++)
		p[i] = guard_byte(i, salt);
}

static int guard_ok(const uint8_t *p, uint32_t salt)
{
	uint32_t i;

	for (i = 0; i < BENCH_GUARD_BYTES; i++)
		if (p[i] != guard_byte(i, salt))
			return 0;
	return 1;
}

/* reps back-to-back calls, timed as a whole; every result is compared with
 * the variant's reference. */
static uint64_t time_block(run_fn run, void *arena, uint32_t reps,
			   bench_vres *v)
{
	const uint64_t ref = bits_of(v->ref);
	uint32_t i, bad = 0;
	double badv = 0.0;
	uint64_t t0, t1;

	t0 = plat_ticks();
	for (i = 0; i < reps; i++) {
		double x = run(arena);

		if (bits_of(x) != ref) {
			if (!bad)
				badv = x;
			bad++;
		}
	}
	t1 = plat_ticks();
	if (bad) {
		if (!v->bad_calls)
			v->bad_value = badv;
		v->bad_calls += bad;
	}
	v->calls += reps;
	return t1 > t0 ? t1 - t0 : 0;
}

static void sort_doubles(double *s, int n)
{
	int i, j;

	for (i = 1; i < n; i++) {
		double x = s[i];

		for (j = i; j > 0 && s[j - 1] > x; j--)
			s[j] = s[j - 1];
		s[j] = x;
	}
}

static void stats(bench_vres *v, int n)
{
	double s[BENCH_MAX_SAMPLES];
	int i;

	if (n <= 0)
		return;
	for (i = 0; i < n; i++)
		s[i] = v->sample[i];
	sort_doubles(s, n);
	v->tmin = s[0];
	v->tmax = s[n - 1];
	v->tmed = (n & 1) ? s[n / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
}

static int is_finite(double x)
{
	return x == x && x - x == 0.0;
}

/* |a-b| <= tol * max(|a|,|b|).  Equal values (including +0/-0) pass; NaN
 * or infinite results never do. */
static int approx_equal(double a, double b, double tol, double *rel)
{
	double m, d;

	if (a == b) {
		*rel = 0.0;
		return is_finite(a);
	}
	if (!is_finite(a) || !is_finite(b)) {
		*rel = HUGE_VAL;
		return 0;
	}
	m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
	d = fabs(a - b);
	*rel = d / m;
	return d <= tol * m;
}

static const char *check_desc(const bench_kernel *k)
{
	if (!k)
		return "NULL descriptor";
	if (!k->name || !k->name[0])
		return "no name";
	if (strlen(k->name) > 8)
		return "name longer than 8";
	if (!k->init || !k->run)
		return "init or run is NULL";
	if (k->category >= BENCH_CAT_COUNT)
		return "bad category";
	if (k->flags > BENCH_F_APPROX)
		return "bad flags";
	if (k->flags == BENCH_F_APPROX && !(k->tolerance > 0.0 && k->tolerance < 1.0))
		return "APPROX tolerance not in (0,1)";
	if (!k->units)
		return "units is 0";
	if (!k->arena_bytes || k->arena_bytes > BENCH_ARENA_BYTES)
		return "arena_bytes out of range";
	return NULL;
}

static int big_endian(void)
{
	const union {
		uint32_t u;
		uint8_t b[4];
	} x = { 1u };

	return x.b[0] == 0;
}

static const bench_ref *find_ref(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(bench_refs) / sizeof(bench_refs[0]); i++)
		if (!strcmp(bench_refs[i].name, name))
			return &bench_refs[i];
	return NULL;
}

/* Each variant's warm-up result against the recorded reference, with the
 * A-vs-B rule.  A reference recorded for another descriptor (the kernel
 * changed since `make refs`) is not used. */
static void check_ref(bench_results *r, bench_kres *e)
{
	const bench_kernel *k = e->k;
	const bench_ref *f = find_ref(k->name);
	int j, exact = k->flags == BENCH_F_EXACT;

	if (!f)
		return;
	if (!(f->flags & BENCH_REF_EXACT) != !exact || f->tol_bits != bits_of(k->tolerance) ||
	    f->units != k->units || f->arena_bytes != k->arena_bytes) {
		e->ref = REF_STALE;
		return;
	}
	if ((f->flags & BENCH_REF_BE_ONLY) && !big_endian()) {
		e->ref = REF_BYTEORDER;
		return;
	}
	e->ref = REF_CHECKED;
	r->nref++;
	{
		dbits x;

		x.u = f->bits;
		e->ref_value = x.d;
	}
	for (j = 0; j < r->nvar; j++) {
		double rel;
		int ok = exact ? bits_of(e->v[j].ref) == f->bits
			       : approx_equal(e->v[j].ref, e->ref_value, k->tolerance, &rel);

		if (!ok)
			e->status |= j ? BST_WRONG_B : BST_WRONG_A;
	}
}

static int same_desc(const bench_kernel *a, const bench_kernel *b)
{
	return a->category == b->category && a->flags == b->flags &&
	       a->units == b->units && a->arena_bytes == b->arena_bytes &&
	       bits_of(a->tolerance) == bits_of(b->tolerance);
}

static int selected(const char *only, const char *name)
{
	size_t n;
	const char *p;

	if (!only || !*only)
		return 1;
	if (!name)
		return 0;
	n = strlen(name);
	for (p = only;;) {
		const char *e = strchr(p, ',');
		size_t l = e ? (size_t)(e - p) : strlen(p);

		if (l == n && !memcmp(p, name, n))
			return 1;
		if (!e)
			return 0;
		p = e + 1;
	}
}

/* ---- one kernel ------------------------------------------------------- */

static void run_kernel(bench_results *r, bench_kres *e, uint8_t *arena,
		       uint64_t target)
{
	const bench_kernel *ka = e->kv[0];
	const int nv = r->nvar;
	const uint32_t ab = ka->arena_bytes;
	uint64_t t, t_one = 0;
	uint32_t reps = 1;
	int s, j, nsamples = r->samples;

	/* 1. same bytes for everybody: poison, guards, one init */
	memset(arena, 0xA5, ab);
	guard_write(arena - BENCH_GUARD_BYTES, 0);
	guard_write(arena + ab, 1);
	ka->init(arena);

	/* 2. warm-up; the results are the references */
	for (j = 0; j < nv; j++) {
		e->v[j].ref = e->kv[j]->run(arena);
		e->v[j].calls = 1;
	}

	/* 3. calibrate with A: double reps until a block takes 1/8 of the
	 * target, then scale. */
	for (;;) {
		t = time_block(ka->run, arena, reps, &e->v[0]);
		if (reps == 1)
			t_one = t;
		if (t >= target / 8 || reps >= REPS_MAX)
			break;
		reps *= 2;
	}
	if (t == 0) {
		e->status |= BST_NOTIME;
	} else {
		double want = (double)reps * (double)target / (double)t;

		if (want < 1.0)
			want = 1.0;
		if (want > (double)REPS_MAX)
			want = (double)REPS_MAX;
		reps = (uint32_t)(want + 0.5);
		/* a single run() far longer than a sample: fewer samples keep
		 * the total run time bounded */
		if (t_one > 4 * target && nsamples > 3)
			nsamples = 3;

		/* 4. samples, alternating the order */
		for (s = 0; s < nsamples; s++) {
			for (j = 0; j < nv; j++) {
				int v = nv == 2 ? (j ^ (s & 1)) : 0;

				t = time_block(e->kv[v]->run, arena, reps, &e->v[v]);
				e->v[v].sample[s] = (double)t / (double)reps;
			}
		}
	}
	e->reps = reps;
	e->samples = (e->status & BST_NOTIME) ? 0 : nsamples;

	/* 6. guards, stability, A vs B */
	if (!guard_ok(arena - BENCH_GUARD_BYTES, 0) || !guard_ok(arena + ab, 1))
		e->status |= BST_OVERRUN;
	for (j = 0; j < nv; j++) {
		stats(&e->v[j], e->samples);
		if (e->v[j].bad_calls)
			e->status |= BST_UNSTABLE;
		if (e->samples && !(e->v[j].tmin > 0.0))
			e->status |= BST_NOTIME;
	}
	if (nv == 2) {
		double a = e->v[0].ref, b = e->v[1].ref, rel = 0.0;
		int same;

		if (ka->flags == BENCH_F_EXACT) {
			same = bits_of(a) == bits_of(b);
			if (!same)
				approx_equal(a, b, 0.0, &rel);
		} else {
			same = approx_equal(a, b, ka->tolerance, &rel);
		}
		e->rel_diff = rel;
		if (!same)
			e->status |= BST_MISMATCH;
		if (e->v[0].tmin > 0.0 && e->v[1].tmin > 0.0)
			e->speedup = e->v[1].tmin / e->v[0].tmin;
	}
	check_ref(r, e);
}

/* ---- all kernels ------------------------------------------------------ */

static int table_len(const bench_variant_info *v)
{
	int n = 0;

	if (!v || !v->kernels)
		return 0;
	while (n < 4 * BENCH_MAX_KERNELS && v->kernels[n])
		n++;
	return n;
}

static void geomeans(bench_results *r)
{
	double lsum[BENCH_CAT_COUNT], lall = 0.0;
	int i, c;

	for (c = 0; c < BENCH_CAT_COUNT; c++) {
		lsum[c] = 0.0;
		r->ngeo[c] = 0;
		r->geo[c] = 0.0;
	}
	r->ngeo_all = 0;
	r->geo_all = 0.0;
	r->nfailed = 0;
	for (i = 0; i < r->nk; i++) {
		const bench_kres *e = &r->k[i];

		if (e->status) {
			r->nfailed++;
			continue;
		}
		if (r->nvar != 2 || !(e->speedup > 0.0) || !is_finite(e->speedup))
			continue;
		c = (int)e->k->category;
		lsum[c] += log(e->speedup);
		r->ngeo[c]++;
		lall += log(e->speedup);
		r->ngeo_all++;
	}
	for (c = 0; c < BENCH_CAT_COUNT; c++)
		if (r->ngeo[c])
			r->geo[c] = exp(lsum[c] / r->ngeo[c]);
	if (r->ngeo_all)
		r->geo_all = exp(lall / r->ngeo_all);
}

int bench_run_all(bench_results *r, const bench_variant_info *const *vars,
		  int nvar, const bench_opts *o,
		  void (*progress)(const bench_results *, int i, int phase),
		  void *arena)
{
	uint64_t t_start, target;
	int i, j, n, nb;

	memset(r, 0, sizeof(*r));
	r->samples = o->samples;
	if (r->samples < 1)
		r->samples = 1;
	if (r->samples > BENCH_MAX_SAMPLES)
		r->samples = BENCH_MAX_SAMPLES;
	r->sample_ms = o->sample_ms > 0 ? o->sample_ms : 1;
	r->quick = o->quick;
	r->tb_hz = plat_ticks_per_sec();
	r->cyc_per_tick = plat_cycles_per_tick();
	if (nvar < 1 || nvar > BENCH_MAX_VARIANTS || !vars || !vars[0]) {
		r->error = "no variant tables";
		return -1;
	}
	r->nvar = nvar;
	for (j = 0; j < nvar; j++) {
		r->var[j] = vars[j];
		if (!vars[j]) {
			r->error = "variant table missing";
			return -1;
		}
	}
	if (!arena) {
		r->error = "no arena";
		return -1;
	}

	n = table_len(vars[0]);
	r->ntable = n;
	if (nvar == 2) {
		nb = table_len(vars[1]);
		if (nb != n) {
			r->error = "A and B kernel tables differ in length";
			if (nb < n)
				n = nb;
		}
	}
	if (n > BENCH_MAX_KERNELS) {
		r->error = "too many kernels";
		n = BENCH_MAX_KERNELS;
	}

	/* the list first, so progress can show i/n */
	for (i = 0; i < n; i++) {
		const bench_kernel *ka = vars[0]->kernels[i];
		bench_kres *e;

		if (!selected(o->only, ka ? ka->name : NULL))
			continue;
		e = &r->k[r->nk++];
		e->index = i;
		e->k = ka;
		for (j = 0; j < nvar; j++)
			e->kv[j] = vars[j]->kernels[i];
	}
	if (!r->nk) {
		if (!r->error)
			r->error = o->only ? "no kernel matches --only" : "no kernels";
		return -1;
	}

	target = (uint64_t)r->sample_ms * r->tb_hz / 1000u;
	if (!target)
		target = 1;
	t_start = plat_ticks();
	for (i = 0; i < r->nk; i++) {
		bench_kres *e = &r->k[i];
		const char *why = NULL;
		uint64_t t0 = plat_ticks();

		if (progress)
			progress(r, i, 0);
		if (e->k && !e->k->init && !e->k->run) {
			e->status |= BST_MISSING;
		} else {
			for (j = 0; j < nvar && !why; j++)
				why = check_desc(e->kv[j]);
			if (!why && nvar == 2) {
				if (strcmp(e->kv[0]->name, e->kv[1]->name))
					why = "A and B names differ";
				else if (!same_desc(e->kv[0], e->kv[1]))
					why = "A and B descriptors differ";
			}
			if (why) {
				e->status |= BST_BADDESC;
				e->note = why;
			} else {
				run_kernel(r, e, (uint8_t *)arena, target);
			}
		}
		e->ticks = plat_ticks() - t0;
		if (progress)
			progress(r, i, 1);
	}
	r->total_ticks = plat_ticks() - t_start;
	geomeans(r);
	return 0;
}
