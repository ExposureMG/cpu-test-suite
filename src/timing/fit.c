/* Timing maths: least squares, median/MAD, shuffle, quality gate. Pure C, host-tested with planted data. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>

double xts_cycles_per_tick = 64.0;

int xts_fit_line(const double *x, const double *y, int n, xts_fit_t *f)
{
  memset(f, 0, sizeof *f);
  f->n = n;
  if (n < 2) return -1;
  double sx = 0, sy = 0;
  for (int i = 0; i < n; i++) { sx += x[i]; sy += y[i]; }
  double mx = sx / n, my = sy / n, sxx = 0, sxy = 0, syy = 0;
  for (int i = 0; i < n; i++) {
    sxx += (x[i] - mx) * (x[i] - mx);
    sxy += (x[i] - mx) * (y[i] - my);
    syy += (y[i] - my) * (y[i] - my);
  }
  if (sxx <= 0) return -1;
  f->slope = sxy / sxx;
  f->intercept = my - f->slope * mx;
  double ssr = 0;
  for (int i = 0; i < n; i++) {
    double r = y[i] - (f->intercept + f->slope * x[i]);
    ssr += r * r;
    if (fabs(r) > f->max_resid) f->max_resid = fabs(r);
    double rel = fabs(y[i]) > 0 ? fabs(r) / fabs(y[i]) : (fabs(r) > 0 ? INFINITY : 0);
    if (rel > f->max_resid_rel) f->max_resid_rel = rel;
  }
  f->r2 = syy > 0 ? 1.0 - ssr / syy : (ssr == 0 ? 1.0 : 0.0);
  f->slope_se = n > 2 ? sqrt(ssr / (n - 2) / sxx) : 0;
  return 0;
}

static int cmp_d(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

double xts_median(const double *v, int n)
{
  if (n <= 0) return NAN;
  double tmp[256];
  if (n > 256) n = 256;
  memcpy(tmp, v, sizeof(double) * n);
  qsort(tmp, n, sizeof(double), cmp_d);
  return (n & 1) ? tmp[n / 2] : 0.5 * (tmp[n / 2 - 1] + tmp[n / 2]);
}

double xts_mad(const double *v, int n)
{
  if (n <= 0) return NAN;
  if (n > 256) n = 256;
  double m = xts_median(v, n), dev[256];
  for (int i = 0; i < n; i++) dev[i] = fabs(v[i] - m);
  return xts_median(dev, n);
}

double xts_min(const double *v, int n)
{
  double m = INFINITY;
  for (int i = 0; i < n; i++)
    if (v[i] < m) m = v[i];
  return n > 0 ? m : NAN;
}

void xts_shuffle(int *idx, int n, uint64_t seed)
{
  xts_rng_t r;
  xts_rng_seed(&r, seed);
  for (int i = n - 1; i > 0; i--) {
    int j = (int)xts_rng_below(&r, (uint32_t)i + 1);
    int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
  }
}

void xts_time_gate(const xts_tspec_t *s, xts_tresult_t *r)
{
  double sl = s->spread_limit > 0 ? s->spread_limit : 0.02, rl = s->resid_limit > 0 ? s->resid_limit : 0.01;
  r->spread_max = 0;
  for (int d = 0; d < s->ndepths; d++) {
    double rel = r->median[d] != 0 ? r->mad[d] / fabs(r->median[d]) : 0;
    if (rel > r->spread_max) r->spread_max = rel;
  }
  r->cycles_per_unit = r->fit.slope * xts_cycles_per_tick;
  r->overhead_cycles = r->fit.intercept * xts_cycles_per_tick;
  r->slope_se = r->fit.slope_se;
  r->err_cycles = NAN;
  if (!r->ok) return;
  int deep = s->ndepths - 1;
  if (deep >= 0 && s->depths[deep] > 0) {          /* the sched_classes error bar: slope SE and deepest-depth MAD */
    double md = r->mad[deep] / s->depths[deep];
    r->err_cycles = xts_cycles_per_tick * sqrt(r->slope_se * r->slope_se + md * md);
    if (r->err_cycles < 0.01) r->err_cycles = 0.01;
  }
  if (r->spread_max < sl && r->fit.max_resid_rel < rl && fabs(r->aba_shift) < sl) {
    r->status = XTS_ST_PASS;
    r->why[0] = 0;
  } else {
    r->status = XTS_ST_NOISY;
    if (r->spread_max >= sl) snprintf(r->why, sizeof r->why, "spread %.4f >= %.4f", r->spread_max, sl);
    else if (r->fit.max_resid_rel >= rl) snprintf(r->why, sizeof r->why, "fit residual %.4f >= %.4f", r->fit.max_resid_rel, rl);
    else snprintf(r->why, sizeof r->why, "A/B/A shift %.4f", r->aba_shift);
  }
}

int xts_time_baseline_check(int insn, const char *key, double value, double rel_tol, double *baseline)
{
  double b;
  if (!xts_isa_baseline(insn, key, &b) || b <= 0) return -1;
  if (baseline) *baseline = b;
  return fabs(value - b) / b > rel_tol ? XTS_ST_REGRESSED : XTS_ST_PASS;
}
