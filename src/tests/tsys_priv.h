/* tsys_priv.h - what branch.c, sysops.c and calib.c share (private to src/tests; included by host/tests/t_tsys_math.c).
 *
 * 1. the pure analysis functions the host tests exercise (types + prototypes);
 * 2. a thin measurement layer over the framework: a measurement (spec + result), producer/consumer/variant meta data, and
 *    three emit wrappers that map it onto xts_time_emit_ex / _diff / _value. All record building, statistics, status
 *    combination and baseline checks are the framework's (xts/timing.h); nothing here formats a record by hand. */
#ifndef XTS_TSYS_PRIV_H
#define XTS_TSYS_PRIV_H
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>

/* ---- pure analysis (branch.c, calib.c) ---- */
typedef struct tsys_ls {
  int ok, jump, S, lo, hi;
  double a, c, M, sse, sse_lin, per_level_lin;
} tsys_ls_t;

typedef struct tsys_cptd {
  double est_dep, est_ind, est, ratio, new_cpt;
  int consistent, set;
  char why[96];
} tsys_cptd_t;

double tsys_mispredict_penalty(double rand_c, double never_c, double always_c);
double tsys_pattern_rate(double pat_c, double pat_never, double pat_always, double rand_c, double rand_never, double rand_always);
int tsys_fit_linkstack(const double *d, const double *y, int n, tsys_ls_t *o);
double tsys_rtc_snap_rate(double rate, double *err_rel);
void tsys_cpt_decide(double slope_dep, double slope_ind, double current, tsys_cptd_t *o);

/* ---- measurements ---- */
typedef struct tmeta {
  const char *producer, *consumer, *variant;
  int distance;                      /* -1: none */
  double per;                        /* instructions (events) per unit: value = cycles per unit / per (0 = 1) */
  int baseline;                      /* 1: also compare with the ISA JSON baseline of the metric */
  const char *xkey;                  /* one extra numeric detail member (NULL: none) */
  double xval;
} tmeta_t;

static inline tmeta_t tm_make(const char *prod, const char *cons, const char *var, double per, int dist)
{
  tmeta_t t = { prod, cons, var, dist, per, 0, NULL, 0 };
  return t;
}

/* a measurement; the unit text is kept inside (callers build it in short-lived buffers) */
typedef struct meas { xts_tspec_t s; xts_tresult_t r; char ubuf[56]; } meas_t;

static inline void mset_unit(meas_t *m, const char *unit)
{
  snprintf(m->ubuf, sizeof m->ubuf, "%s", unit ? unit : "");
  m->s.unit = m->ubuf;
}

/* run the series; errors are recorded in m->r (failed / why) and become fail records at emit time */
static inline int mrun(meas_t *m, xts_ctx_t *ctx) { return xts_time_measure(&m->s, ctx, &m->r); }
static inline int mstatus(const meas_t *m) { return m->r.failed ? XTS_ST_FAIL : m->r.status; }
static inline double mcyc(const meas_t *m) { return xts_time_cycles(&m->r, 1); }

/* iterations so that the smallest default depth (4 units) runs about 4000 timebase ticks: the +-1 tick granularity stays
 * far below the 2 % quality gate */
static inline uint32_t pick_iters(double est_cycles_per_unit)
{
  double cpt = xts_cycles_per_tick > 1 ? xts_cycles_per_tick : 64;
  double it = 4000.0 * cpt / (4 * est_cycles_per_unit + 12);
  if (it < 64) it = 64;
  if (it > 200000) it = 200000;
  return (uint32_t)it;
}

/* ---- records ---- */
typedef struct tdet { const tmeta_t *tm; const char *note; } tdet_t;

/* detail members: producer / consumer / variant / distance / insns_per_unit / one extra number, and an optional note */
static inline void j_tdet(xts_json_t *j, void *arg)
{
  const tdet_t *d = arg;
  const tmeta_t *tm = d ? d->tm : NULL;
  if (tm) {
    if (tm->producer) xj_kv_str(j, "producer", tm->producer);
    if (tm->consumer) xj_kv_str(j, "consumer", tm->consumer);
    if (tm->variant) xj_kv_str(j, "variant", tm->variant);
    if (tm->distance >= 0) xj_kv_int(j, "distance", tm->distance);
    if (tm->per > 1) xj_kv_double(j, "insns_per_unit", tm->per);
    if (tm->xkey) xj_kv_double(j, tm->xkey, tm->xval);
  }
  if (d && d->note) xj_kv_str(j, "note", d->note);
}

static inline double tper(const tmeta_t *tm) { return tm && tm->per > 0 ? tm->per : 1.0; }

/* The case id of every record is "<case id>/<sub>". `word` 0 = no instruction word. */

/* value = cycles per unit / tm->per, from one measurement */
static inline void emit_meas(xts_ctx_t *ctx, const char *sub, const char *metric, int insn, uint32_t word, const meas_t *m,
                             const tmeta_t *tm)
{
  tdet_t d = { tm, NULL };
  xts_time_emit_ex(ctx, &m->s, &m->r, insn,
                   &(xts_temit_t){ .metric = metric, .per_unit = tper(tm), .baseline_key = tm && tm->baseline ? "*" : NULL,
                                   .word = word, .case_suffix = sub, .detail_fn = j_tdet, .detail_arg = &d });
}

/* value = (a - b) cycles per unit / tm->per; both fits are in the detail ("fit", "ref") */
static inline void emit_diff(xts_ctx_t *ctx, const char *sub, const char *metric, int insn, uint32_t word, const meas_t *a,
                             const meas_t *b, const tmeta_t *tm)
{
  tdet_t d = { tm, NULL };
  xts_time_emit_diff(ctx, &a->s, &a->r, &b->s, &b->r, insn,
                     &(xts_temit_t){ .metric = metric, .per_unit = tper(tm), .word = word, .case_suffix = sub,
                                     .detail_fn = j_tdet, .detail_arg = &d });
}

/* a value computed by the caller from one or two measurements. `m` (may be NULL) supplies the fit shown in the detail and
 * its status; `status` is whatever the caller derived from the other measurements (combined with xts_status_worst) */
static inline void emit_val(xts_ctx_t *ctx, const char *sub, const char *metric, const char *unit, int insn, uint32_t word,
                            double value, int status, const meas_t *m, const tmeta_t *tm, const char *note)
{
  tdet_t d = { tm, note };
  xts_time_emit_value(ctx, m ? &m->s : NULL, m ? &m->r : NULL, insn, value,
                      &(xts_temit_t){ .metric = metric, .unit = unit, .word = word, .case_suffix = sub, .status = status,
                                      .detail_fn = j_tdet, .detail_arg = &d });
}
#endif
