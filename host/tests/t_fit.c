/* timing maths with planted data */
#include <math.h>
#include <xts/xts.h>
#include "htest.h"

HTEST(fit_math)
{
  double x[8], y[8];
  xts_rng_t r;
  xts_rng_seed(&r, 7);
  for (int i = 0; i < 8; i++) {
    x[i] = 4 + 4 * i;
    y[i] = 3.5 + 2.0 / 64 * x[i] * 64 + ((double)xts_rng_below(&r, 1000) - 500) * 1e-6;   /* slope 2, intercept 3.5 */
  }
  xts_fit_t f;
  HT_EQ_U64(xts_fit_line(x, y, 8, &f), 0);
  HT_NEAR(f.slope, 2.0, 1e-3);
  HT_NEAR(f.intercept, 3.5, 1e-2);
  HT_CHECK(f.r2 > 0.99999);
  HT_CHECK(f.max_resid_rel < 1e-3);
  double flat[3] = { 1, 1, 1 };
  HT_CHECK(xts_fit_line(flat, y, 3, &f) < 0);              /* degenerate x */
  double v[7] = { 5, 1, 4, 2, 3, 100, 3 };
  HT_NEAR(xts_median(v, 7), 3, 0);
  HT_NEAR(xts_mad(v, 7), 1, 0);
  HT_NEAR(xts_min(v, 7), 1, 0);
  double even[4] = { 4, 1, 3, 2 };
  HT_NEAR(xts_median(even, 4), 2.5, 0);
  int idx[10], idx2[10];
  for (int i = 0; i < 10; i++) idx[i] = idx2[i] = i;
  xts_shuffle(idx, 10, 99);
  xts_shuffle(idx2, 10, 99);
  int same = 1, sum = 0;
  for (int i = 0; i < 10; i++) { same &= idx[i] == idx2[i]; sum += idx[i]; }
  HT_CHECK(same);
  HT_EQ_U64(sum, 45);
  /* quality gate */
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  xts_tresult_t res = { 0 };
  res.ok = 1;
  for (int d = 0; d < s.ndepths; d++) {
    x[d] = s.depths[d];
    res.median[d] = 10 + 2 * s.depths[d];
    res.mad[d] = 0.001 * res.median[d];
  }
  xts_fit_line(x, res.median, s.ndepths, &res.fit);
  xts_time_gate(&s, &res);
  HT_EQ_U64(res.status, XTS_ST_PASS);
  HT_NEAR(res.cycles_per_unit, 2 * xts_cycles_per_tick, 1e-9);
  res.mad[2] = 0.05 * res.median[2];                         /* one noisy depth */
  xts_time_gate(&s, &res);
  HT_EQ_U64(res.status, XTS_ST_NOISY);
  res.mad[2] = 0;
  res.median[3] *= 1.05;                                     /* a bent but repeatable point: structure, not noise */
  xts_fit_line(x, res.median, s.ndepths, &res.fit);
  xts_time_gate(&s, &res);
  HT_EQ_U64(res.status, XTS_ST_NONLINEAR);
  HT_CHECK(res.resid_over_noise >= XTS_NONLINEAR_FACTOR);
  for (int d = 0; d < s.ndepths; d++) res.mad[d] = 0.012 * res.median[d];   /* same bend inside the repeat spread */
  xts_time_gate(&s, &res);
  HT_EQ_U64(res.status, XTS_ST_NOISY);
  for (int d = 0; d < s.ndepths; d++) res.mad[d] = 0;          /* MAD 0: the 1/iters tick floor still applies */
  res.aba_shift = 0.05;                                      /* but a regime shift is noise */
  xts_time_gate(&s, &res);
  HT_EQ_U64(res.status, XTS_ST_NOISY);
  res.aba_shift = 0;
  xts_time_gate(&s, &res);
  HT_EQ_U64(res.status, XTS_ST_NONLINEAR);
  double base;
  HT_EQ_U64(xts_time_baseline_check(XI_addx, "latency", 2.02, 0.1, &base), XTS_ST_PASS);
  HT_EQ_U64(xts_time_baseline_check(XI_addx, "latency", 3.0, 0.1, &base), XTS_ST_REGRESSED);
  HT_EQ_U64(xts_time_baseline_check(XI_addx, "no_such_key", 3.0, 0.1, &base), (uint64_t)-1);
}

static int body_add(xts_kbuf_t *k, int depth, void *arg)
{
  (void)arg;
  for (int i = 0; i < depth; i++) xk_emit(k, XI_addx, 0, 3, 3, 4);
  return 0;
}

HTEST(timing_frame_host)
{
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  s.body = body_add;
  s.unit = "addx";
  xts_kbuf_t *k = xk_new(4096);
  HT_EQ_U64(xts_time_build(&s, 8, k), 0);
  int ni, nr;
  char why[96];
  HT_EQ_U64(xk_validate(k, &ni, &nr, why, sizeof why), 0);
  int adds = 0, mftb = 0;
  for (uint32_t i = 0; i < k->n; i++) { adds += k->tag[i] == XI_addx; mftb += k->tag[i] == XI_mftb; }
  HT_EQ_U64(adds, 8);
  HT_EQ_U64(mftb, 2);
  xts_tresult_t r;
  HT_EQ_U64(xts_time_measure(&s, NULL, &r), 0);              /* host: builds every depth, runs nothing */
  HT_EQ_U64(r.status, XTS_ST_SKIP);
  HT_EQ_U64(r.nruns, s.ndepths);
  xk_free(k);
}
