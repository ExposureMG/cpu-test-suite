/* Timing engine: framed loop kernels at several depths, shuffled repeats, A/B/A, least-squares fit, quality gate, and the
 * one timing-record emitter (xts_time_emit_ex / _diff / _value; xts_time_emit is the v1 wrapper). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>

#define R_ITERS 29     /* iteration count register */
#define R_T0 27
#define R_T1 28

int xts_time_build(const xts_tspec_t *s, int depth, xts_kbuf_t *k)
{
  int gpr = s->loop == XTS_LOOP_GPR;
  int crf = s->loop_crf >= 1 && s->loop_crf <= 7 ? s->loop_crf : 7;
  xk_reset(k);
  if (s->loop != XTS_LOOP_CTR && !gpr) {
    snprintf(k->errmsg, sizeof k->errmsg, "bad tspec.loop %d", s->loop);
    k->err = -1;
    return -1;
  }
  xk_prio_prologue(k);
  if (s->pre && s->pre(k, depth, s->arg) < 0 && !k->err) {
    snprintf(k->errmsg, sizeof k->errmsg, "pre() failed");
    k->err = -1;
  }
  if (!gpr) xk_emit(k, XI_mtspr, 0, 9, R_ITERS);     /* mtctr r29 */
  xk_emit(k, XI_isync, 0);
  xk_emit(k, XI_mftb, 0, R_T0, 268);
  xk_align(k, s->align ? s->align : 32);
  for (int i = 0; i < (s->top_offset & 0x7C) / 4; i++) xk_emit(k, XI_ori, 0, 0, 0, 0);   /* nop */
  int top = xk_here(k);
  if (s->body(k, depth, s->arg) < 0 && !k->err) {
    snprintf(k->errmsg, sizeof k->errmsg, "body() failed at depth %d", depth);
    k->err = -1;
  }
  xk_loop_close(k, top, gpr ? XTS_LOOP_GPR : XTS_LOOP_CTR, R_ITERS, crf);   /* bdnz top (CTR mode, in reach) */
  xk_emit(k, XI_mftb, 0, R_T1, 268);
  xk_exit(k);
  return xk_finish(k);
}

static xts_kbuf_t *shared_k;

static int do_setup(const xts_tspec_t *s, xts_exec_t *x, int depth, int run)
{
  if (s->setup2) return s->setup2(x, depth, run, s->arg);
  if (s->setup) return s->setup(x, s->arg);
  return 0;
}

int xts_time_measure(const xts_tspec_t *s, xts_ctx_t *ctx, xts_tresult_t *r)
{
  memset(r, 0, sizeof *r);
  r->status = XTS_ST_SKIP;
  r->err_cycles = NAN;
  r->failed = 1;                                      /* cleared on every successful return */
  if (!s->body || s->ndepths < 2 || s->ndepths > XTS_TMAX_DEPTHS || s->repeats < 1 || s->repeats > XTS_TMAX_REPEATS ||
      !s->iters) {
    snprintf(r->why, sizeof r->why, "bad tspec");
    return -1;
  }
  if (!shared_k) shared_k = xk_new(xts_exec_kernel_capacity());
  if (!shared_k) { snprintf(r->why, sizeof r->why, "out of memory"); return -1; }
  static xts_exec_t x;
  int nd = s->ndepths, nrep = s->repeats, nruns = nd * nrep;
  static double vals[XTS_TMAX_DEPTHS][XTS_TMAX_REPEATS];
  int order[XTS_TMAX_DEPTHS * XTS_TMAX_REPEATS];
  int cnt[XTS_TMAX_DEPTHS] = { 0 };
  for (int i = 0; i < nruns; i++) order[i] = i % nd;
  xts_shuffle(order, nruns, ctx ? ctx->seed : 1);
  if (xts_exec_is_host()) {                           /* host: build + validate every depth once, run nothing */
    for (int d = 0; d < nd; d++) {
      if (xts_time_build(s, s->depths[d], shared_k) < 0) {
        snprintf(r->why, sizeof r->why, "kernel build (depth %d): %s", s->depths[d], shared_k->errmsg);
        return -1;
      }
      if ((int)shared_k->n > r->kernel_words_max) r->kernel_words_max = (int)shared_k->n;
      xts_case_init(&x);
      if (do_setup(s, &x, s->depths[d], d) < 0) { snprintf(r->why, sizeof r->why, "setup() failed"); return -1; }
      x.in.gpr[R_ITERS] = s->iters;
      if (xts_exec_kernel(&x, shared_k) < 0) { snprintf(r->why, sizeof r->why, "%s", x.err); return -1; }
      r->nruns++;
    }
    snprintf(r->why, sizeof r->why, "not executed (host dry run)");
    r->failed = 0;
    return 0;
  }
  r->ok = 1;
  /* run -1: warm-up of depths[0], not recorded (cold data, TLB and predictors made the first run of a series slow).
   * runs nruns .. nruns+XTS_ABA_RUNS-1: the A/B/A re-runs of depths[0]. A/B/A compares MINIMA (the re-runs against
   * depths[0]'s repeats): a one-off event only adds time, a regime shift moves the minimum too. */
  double aba[XTS_ABA_RUNS];
  int naba = 0;
  for (int i = -1; i < nruns + XTS_ABA_RUNS; i++) {
    int d = i >= 0 && i < nruns ? order[i] : 0;
    int run = i < 0 ? 0 : i < nruns ? i : nruns;      /* setup2's run index: the A/B/A re-runs are run nruns */
    if (xts_time_build(s, s->depths[d], shared_k) < 0) {
      snprintf(r->why, sizeof r->why, "kernel build (depth %d): %s", s->depths[d], shared_k->errmsg);
      r->ok = 0;
      return -1;
    }
    if ((int)shared_k->n > r->kernel_words_max) r->kernel_words_max = (int)shared_k->n;
    xts_case_init(&x);
    if (do_setup(s, &x, s->depths[d], run) < 0) { snprintf(r->why, sizeof r->why, "setup() failed"); r->ok = 0; return -1; }
    x.in.gpr[R_ITERS] = s->iters;
    int st = xts_exec_kernel(&x, shared_k);
    r->nruns++;
    if (st != XTS_EXEC_OK) { snprintf(r->why, sizeof r->why, "exec: %s", x.err); r->ok = 0; return -1; }
    if (x.trap.trapped) {
      snprintf(r->why, sizeof r->why, "trap vector %X cause %02X at kernel+%u", x.trap.vector, x.trap.cause, x.trap.offset);
      r->ok = 0;
      r->status = XTS_ST_TRAP;
      r->failed = 0;
      return 0;
    }
    double ticks = (double)(uint32_t)(x.out.gpr[R_T1] - x.out.gpr[R_T0]);
    double y = ticks / (double)s->iters;
    if (i >= 0 && i < nruns) vals[d][cnt[d]++] = y;
    else if (i >= nruns) aba[naba++] = y;
  }
  double xs[XTS_TMAX_DEPTHS], ys[XTS_TMAX_DEPTHS];
  for (int d = 0; d < nd; d++) {
    r->median[d] = xts_median(vals[d], cnt[d]);
    r->mad[d] = xts_mad(vals[d], cnt[d]);
    r->minv[d] = xts_min(vals[d], cnt[d]);
    xs[d] = s->depths[d];
    ys[d] = r->median[d];
  }
  double first0 = r->minv[0], last0 = xts_min(aba, naba);
  r->aba_shift = first0 != 0 ? (last0 - first0) / first0 : 0;
  double sl = s->spread_limit > 0 ? s->spread_limit : 0.02;
  r->outliers = 0;                                    /* diagnostics: repeats more than the spread limit above their median */
  for (int d = 0; d < nd; d++)
    for (int k = 0; k < cnt[d]; k++)
      if (vals[d][k] > r->median[d] * (1 + sl)) r->outliers++;
  if (xts_fit_line(xs, ys, nd, &r->fit) < 0) { snprintf(r->why, sizeof r->why, "degenerate depths"); r->ok = 0; return -1; }
  xts_time_gate(s, r);
  r->failed = 0;
  return 0;
}

int xts_time_differential(const xts_tspec_t *a, const xts_tspec_t *filler, xts_ctx_t *ctx, xts_tresult_t *ra,
                          xts_tresult_t *rf, double *diff)
{
  int e = xts_time_measure(a, ctx, ra);
  if (e < 0) return e;
  e = xts_time_measure(filler, ctx, rf);
  if (e < 0) return e;
  if (diff) *diff = (ra->ok && rf->ok) ? ra->cycles_per_unit - rf->cycles_per_unit : NAN;
  return 0;
}

/* ---- records ---------------------------------------------------------------------------------------------------------- */
double xts_time_cycles(const xts_tresult_t *r, double per_unit)
{
  if (!r || !r->ok || r->failed) return NAN;
  return r->cycles_per_unit / (per_unit > 0 ? per_unit : 1.0);
}

const char *xts_time_baseline_key(const char *metric)
{
  if (!metric) return "latency";
  if (!strcmp(metric, "rthroughput") || !strcmp(metric, "issue_cost")) return "issue";
  return metric;
}

static int res_status(const xts_tresult_t *r) { return !r ? XTS_ST_PASS : r->failed ? XTS_ST_FAIL : r->status; }

void xts_time_fit_json(xts_json_t *j, const char *key, const xts_tspec_t *s, const xts_tresult_t *r, double per_unit)
{
  int ok = r && r->ok && !r->failed;
  int nd = s ? s->ndepths : 0;
  if (nd < 0) nd = 0;
  if (nd > XTS_TMAX_DEPTHS) nd = XTS_TMAX_DEPTHS;
  xj_key(j, key);
  xj_obj(j);
  xj_kv_str(j, "unit", s && s->unit ? s->unit : "");
  xj_kv_uint(j, "iters", s ? s->iters : 0);
  xj_kv_str(j, "status", xts_status_name(res_status(r)));
  if (r && r->why[0]) xj_kv_str(j, "why", r->why);
  xj_kv_double(j, "cpu", ok ? r->cycles_per_unit : NAN);
  xj_kv_double(j, "slope_ticks", ok ? r->fit.slope : NAN);
  xj_kv_double(j, "intercept_ticks", ok ? r->fit.intercept : NAN);
  xj_kv_double(j, "r2", ok ? r->fit.r2 : NAN);
  xj_kv_double(j, "max_resid_rel", ok ? r->fit.max_resid_rel : NAN);
  xj_kv_double(j, "spread", ok ? r->spread_max : NAN);
  xj_kv_double(j, "aba_shift", ok ? r->aba_shift : NAN);
  xj_kv_double(j, "slope_se", ok ? r->slope_se : NAN);
  xj_kv_double(j, "resid_over_noise", ok ? r->resid_over_noise : NAN);
  xj_kv_int(j, "outliers", ok ? r->outliers : -1);
  xj_key(j, "depths");
  xj_arr(j);
  for (int d = 0; d < nd; d++) xj_int(j, s->depths[d]);
  xj_end(j);
  xj_key(j, "ticks_med");
  xj_arr(j);
  for (int d = 0; d < nd; d++) xj_double(j, ok ? r->median[d] : NAN);
  xj_end(j);
  xj_key(j, "ticks_mad");
  xj_arr(j);
  for (int d = 0; d < nd; d++) xj_double(j, ok ? r->mad[d] : NAN);
  xj_end(j);
  /* cost of each depth step (cycles per unit, divided by per_unit like the record value): what a nonlinear fit averages */
  xj_key(j, "marginal_cycles");
  xj_arr(j);
  for (int d = 0; d + 1 < nd; d++) {
    int dd = s->depths[d + 1] - s->depths[d];
    xj_double(j, ok && dd > 0 ? (r->median[d + 1] - r->median[d]) / dd * xts_cycles_per_tick / (per_unit > 0 ? per_unit : 1.0) : NAN);
  }
  xj_end(j);
  xj_kv_double(j, "cycles_per_tick", xts_cycles_per_tick);
  xj_kv_double(j, "per_unit", per_unit > 0 ? per_unit : 1.0);
  xj_kv_int(j, "kernel_words_max", r ? r->kernel_words_max : 0);
  xj_end(j);
}

typedef struct emit_in {
  const xts_tspec_t *sa, *sb;
  const xts_tresult_t *ra, *rb;
  int insn;
  int diff;            /* 1: a - b */
  int value_only;      /* 1: value given by the caller (xts_time_emit_value) */
  double value;
} emit_in_t;

static void emit_core(xts_ctx_t *ctx, const emit_in_t *in, const xts_temit_t *e)
{
  static char buf[16384], cid[96];   /* detail; must stay below the sink record size and xts_results_foreach line (32 KB) */
  xts_json_t j;
  double per = e->per_unit > 0 ? e->per_unit : 1.0;
  const xts_tspec_t *s = in->sa;
  const xts_tresult_t *r = in->ra;
  int okr = r && s && r->ok && !r->failed;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_members(&j, e->extra_json);
  if (e->detail_fn) e->detail_fn(&j, e->detail_arg);
  if (s && r) xts_time_fit_json(&j, "fit", s, r, per);
  if (in->diff && in->sb && in->rb) xts_time_fit_json(&j, "ref", in->sb, in->rb, per);

  /* value */
  double cyc = okr ? r->cycles_per_unit / per : NAN, value;
  int per_unit_value;
  if (in->value_only) value = in->value;
  else if (e->has_value) value = e->value;
  else if (in->diff) {
    double cb = xts_time_cycles(in->rb, per);
    value = (okr && !isnan(cb)) ? cyc - cb : NAN;
  } else value = cyc;
  per_unit_value = !in->diff && okr && !isnan(value) && fabs(value - cyc) <= 1e-9 * (fabs(cyc) > 1 ? fabs(cyc) : 1);

  /* status */
  int status = XTS_ST_PASS;
  if (s && r) status = res_status(r);
  if (in->diff) status = xts_status_worst(status, res_status(in->rb));
  if (r && r->failed) xj_kv_str(&j, "error", r->why);
  if (in->diff && in->rb && in->rb->failed) xj_kv_str(&j, "error_ref", in->rb->why);
  if (e->baseline_key && status == XTS_ST_PASS && in->insn >= 0 && !isnan(value)) {
    const char *key = !strcmp(e->baseline_key, "*") ? xts_time_baseline_key(e->metric) : e->baseline_key;
    double base;
    int b = xts_time_baseline_check(in->insn, key, value, e->baseline_tol > 0 ? e->baseline_tol : 0.10, &base);
    if (b >= 0) {
      xj_kv_double(&j, "baseline", base);
      xj_kv_str(&j, "baseline_key", key);
      if (b == XTS_ST_REGRESSED) status = XTS_ST_REGRESSED;
    }
  }
  int measured = status == XTS_ST_PASS || status == XTS_ST_NONLINEAR;   /* implausible values downgrade both */
  if (measured && !isnan(value) && e->floor > 0 && value < e->floor * 0.97) {
    status = XTS_ST_NOISY;
    xj_kv_str(&j, "implausible", "below the physical floor");
    xj_kv_double(&j, "floor", e->floor);
  }
  if (measured && status != XTS_ST_NOISY && !isnan(value) && e->no_negative && value < -0.5) {
    status = XTS_ST_NOISY;
    xj_kv_str(&j, "implausible", "negative cost");
  }
  status = xts_status_worst(status, e->status);
  xj_finish(&j);

  xts_record_t rec;
  xts_record_init(&rec);
  if (in->insn >= 0) rec.mnem = xts_insn_name(in->insn);
  if (e->case_id) rec.case_id = e->case_id;
  else if (e->case_suffix && ctx && ctx->c) {
    snprintf(cid, sizeof cid, "%s/%s", ctx->c->id, e->case_suffix);
    rec.case_id = cid;
  }
  rec.word = e->word;
  rec.has_word = e->word != 0;
  rec.metric = e->metric ? e->metric : "latency";
  rec.unit = e->unit ? e->unit : "cycles";
  rec.value = value;
  if (okr) {
    rec.n = r->nruns + (in->diff && in->rb ? in->rb->nruns : 0);
    rec.fit_r2 = r->fit.r2;
    if (in->diff && in->rb && in->rb->ok && !in->rb->failed && in->rb->fit.r2 < rec.fit_r2) rec.fit_r2 = in->rb->fit.r2;
    int deep = s->ndepths - 1;
    if (per_unit_value && deep >= 0 && s->depths[deep] > 0) {
      rec.min = (r->minv[deep] - r->fit.intercept) / s->depths[deep] * xts_cycles_per_tick / per;
      rec.median = cyc;
      rec.mad = r->mad[deep] / s->depths[deep] * xts_cycles_per_tick / per;
    }
  } else if (r) {
    rec.n = r->nruns;
  }
  rec.status = status;
  rec.detail = buf;
  rec.detail_truncated = j.truncated;
  xts_emit(ctx, &rec);
}

void xts_time_emit_ex(xts_ctx_t *ctx, const xts_tspec_t *s, const xts_tresult_t *r, int insn, const xts_temit_t *e)
{
  emit_in_t in = { .sa = s, .ra = r, .insn = insn };
  emit_core(ctx, &in, e);
}

void xts_time_emit_diff(xts_ctx_t *ctx, const xts_tspec_t *sa, const xts_tresult_t *ra, const xts_tspec_t *sb,
                        const xts_tresult_t *rb, int insn, const xts_temit_t *e)
{
  emit_in_t in = { .sa = sa, .ra = ra, .sb = sb ? sb : sa, .rb = rb, .insn = insn, .diff = 1 };
  emit_core(ctx, &in, e);
}

void xts_time_emit_value(xts_ctx_t *ctx, const xts_tspec_t *s, const xts_tresult_t *r, int insn, double value,
                         const xts_temit_t *e)
{
  emit_in_t in = { .sa = s, .ra = r, .insn = insn, .value_only = 1, .value = value };
  emit_core(ctx, &in, e);
}

void xts_time_emit(xts_ctx_t *ctx, const xts_tspec_t *s, const xts_tresult_t *r, const char *metric, int insn)
{
  xts_temit_t e = { .metric = metric, .baseline_key = metric };
  xts_time_emit_ex(ctx, s, r, insn, &e);
}
