/* issue.pair - which instruction classes issue together, per ORDER (slot dependence), against a real 1-cycle filler.
 *
 * Every kernel has the same shape: 2 words per depth unit, independent instructions only (rotating destinations, constant
 * sources; the two members use different register banks, CR fields and store lines), fitted over 6 depths.
 *   control.filler  li rF,imm streams at 1 and 2 words/unit must run at 1.00 cycle per word (+-5 %): the filler is a real
 *                   independent 1-cycle instruction (the old `ori 0,0,0` was a dependent chain through r0). If this
 *                   fails every pair verdict below is UNDETERMINED.
 *   alone.<C>       C C per unit -> T(C) = issue cost per instruction (metric issue_cost); checked against the JSON issue
 *                   baseline where there is one (regressed if off by > 10 %).
 *   pair.<A>+<B>    A B and B A per unit -> P(AB), P(BA) (metric cycles, per pair). overlap = (T(A)+T(B)-P)/min(T(A),T(B))
 *                   with a propagated error bar: >= 0.8 co-issue "yes", <= 0.2 "no", else "partial"; UNDETERMINED when the
 *                   class changes inside +-2 sigma or a measurement is noisy; "faster" (P below the slower member alone)
 *                   and "interfere" (P above serial) are reported as anomalies. order_dependent compares AB with BA.
 *                   Diagonal A+A must equal 2 T(A) (diag_ok).
 * T(A)/T(B) come from the alone cases of the same condition vector and repeat (cached), measured on demand if missing. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sched_classes.h"

#define MAXC 64
typedef struct { uint64_t key; int valid; double cyc, err; int status; } ip_cache_t;
static ip_cache_t alone[MAXC], ctrl;
static int ctrl_ok;

static int ip_enum(xts_caselist_t *cl)
{
  sc_classes_init();
  int idx[MAXC], n = 0;
  for (int i = 0; i < sc_nclasses && n < MAXC; i++)
    if (sc_class_ok(i) && (sc_classes[i].use & SC_U_PAIR)) idx[n++] = i;
  xts_case_t *c = xts_case_add(cl, "control.filler");
  if (!c) return -1;
  c->arg[0] = 0;
  for (int i = 0; i < n; i++) {
    if (!(c = xts_case_addf(cl, "alone.%s", sc_classes[idx[i]].name))) return -1;
    c->arg[0] = 1;
    c->arg[1] = idx[i];
    c->insn = sc_classes[idx[i]].insn;
  }
  for (int i = 0; i < n; i++)
    for (int k = i; k < n; k++) {
      if (!(c = xts_case_addf(cl, "pair.%s+%s", sc_classes[idx[i]].name, sc_classes[idx[k]].name))) return -1;
      c->arg[0] = 2;
      c->arg[1] = idx[i];
      c->arg[2] = idx[k];
    }
  return 0;
}

static int measure_alone(xts_ctx_t *ctx, int ci, sc_meas_t *m)
{
  sc_unit_t u;
  sc_unit_init(&u);
  sc_unit_add(&u, ci, 0, 2);
  return sc_measure(ctx, &u, 2 * sc_class_est(&sc_classes[ci]), m);
}

static void cache_put(ip_cache_t *c, uint64_t key, const sc_meas_t *m, double scale)
{
  c->key = key;
  c->valid = 1;
  c->cyc = m->cyc * scale;
  c->err = m->err * scale;
  c->status = m->status;
}

static int run_control(xts_ctx_t *ctx, int emit)
{
  static sc_meas_t m1, m2;
  sc_unit_t u;
  sc_unit_init(&u);
  sc_unit_add(&u, SC_FILLER, 0, 1);
  sc_measure(ctx, &u, 1, &m1);
  sc_unit_init(&u);
  sc_unit_add(&u, SC_FILLER, 0, 2);
  sc_measure(ctx, &u, 2, &m2);
  int st = xts_status_worst(m1.status, m2.status);       /* a failed measurement is FAIL */
  double t1 = m1.cyc, t2 = m2.cyc / 2;
  int ok = m1.ok && m2.ok && fabs(t1 - 1.0) <= 0.05 && fabs(t2 - 1.0) <= 0.05;
  ctrl_ok = ok;
  cache_put(&ctrl, sc_cond_key(ctx), &m2, 0.5);
  if (!emit) return 0;
  char buf[2400];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "test", "li rF,imm stream (no register reads, 4 rotating destinations)");
  xj_kv_double(&j, "per_word_1", t1);
  xj_kv_double(&j, "per_word_2", t2);
  xj_kv_double(&j, "expected", 1.0);
  xj_kv_double(&j, "tolerance", 0.05);
  xj_kv_str(&j, "verdict", m1.ok && m2.ok ? (ok ? "1-cycle filler confirmed" : "FILLER NOT 1/CYCLE") : "UNDETERMINED");
  xj_kv_double(&j, "err", m1.err);
  xts_time_fit_json(&j, "fit2", &m2.s, &m2.r, 2);         /* "fit" is the 1 word/unit stream */
  xj_finish(&j);
  if (st == XTS_ST_PASS && !ok) st = XTS_ST_FAIL;
  xts_time_emit_ex(ctx, &m1.s, &m1.r, XI_addi, &(xts_temit_t){ .metric = "issue_cost", .extra_json = buf, .status = st });
  return 0;
}

static const ip_cache_t *get_alone(xts_ctx_t *ctx, int ci, int *lazy)
{
  uint64_t key = sc_cond_key(ctx);
  if (ci < MAXC && alone[ci].valid && alone[ci].key == key) return &alone[ci];
  static sc_meas_t m;
  measure_alone(ctx, ci, &m);
  cache_put(&alone[ci], key, &m, 0.5);
  if (lazy) *lazy = 1;
  return &alone[ci];
}

static void ensure_control(xts_ctx_t *ctx)
{
  if (!(ctrl.valid && ctrl.key == sc_cond_key(ctx))) run_control(ctx, 0);
}

static int run_alone(xts_ctx_t *ctx, int ci)
{
  static sc_meas_t m;
  const sc_class_t *c = &sc_classes[ci];
  measure_alone(ctx, ci, &m);
  cache_put(&alone[ci], sc_cond_key(ctx), &m, 0.5);
  char buf[600];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "class", c->name);
  xj_kv_str(&j, "desc", c->desc);
  xj_kv_str(&j, "shape", "C C per unit, independent (rotating destinations)");
  xj_kv_double(&j, "err", m.err / 2);
  xj_finish(&j);
  xts_time_emit_ex(ctx, &m.s, &m.r, c->insn, &(xts_temit_t){ .metric = "issue_cost", .per_unit = 2, .baseline_key = "*",
                                                              .extra_json = buf });
  return 0;
}

static const char *classify(double P, double eP, const ip_cache_t *a, const ip_cache_t *b, int st, double *ov_out,
                            double *err_out, const char **why)
{
  *ov_out = *err_out = NAN;
  *why = NULL;
  if (st == XTS_ST_SKIP) { *why = "not executed (host)"; return "UNDETERMINED"; }
  if (st != XTS_ST_PASS) { *why = "a measurement did not pass the quality gate"; return "UNDETERMINED"; }
  if (!ctrl_ok) { *why = "filler control failed"; return "UNDETERMINED"; }
  double ta = a->cyc, tb = b->cyc, mn = fmin(ta, tb), mx = fmax(ta, tb);
  if (!(mn > 0)) { *why = "alone cost missing"; return "UNDETERMINED"; }
  double err = (eP + a->err + b->err);
  double ov = (ta + tb - P) / mn, eo = err / mn;
  *ov_out = ov;
  *err_out = eo;
  if (P < mx - 2 * err) { *why = "pair faster than its slower member alone"; return "anomaly_faster"; }
  if (P > ta + tb + 2 * err) { *why = "pair slower than serial: the members interfere"; return "interfere"; }
  const char *c0 = sc_tri(ov, 0.8, 0.2, "yes", "no", "partial");
  const char *c1 = sc_tri(ov - 2 * eo, 0.8, 0.2, "yes", "no", "partial");
  const char *c2 = sc_tri(ov + 2 * eo, 0.8, 0.2, "yes", "no", "partial");
  if (strcmp(c0, c1) || strcmp(c0, c2)) { *why = "classification changes within the error bar"; return "UNDETERMINED"; }
  return c0;
}

static int run_pair(xts_ctx_t *ctx, int ca, int cb)
{
  static sc_meas_t m[2];
  int lazy = 0;
  ensure_control(ctx);
  const ip_cache_t *ta = get_alone(ctx, ca, &lazy), *tb = get_alone(ctx, cb, &lazy);
  for (int o = 0; o < 2; o++) {
    sc_unit_t u;
    sc_unit_init(&u);
    sc_unit_add(&u, o ? cb : ca, 0, 1);
    sc_unit_add(&u, o ? ca : cb, 1, 1);
    sc_measure(ctx, &u, sc_class_est(&sc_classes[ca]) + sc_class_est(&sc_classes[cb]), &m[o]);
  }
  double d = m[0].cyc - m[1].cyc, de = sqrt(m[0].err * m[0].err + m[1].err * m[1].err);
  int all_st = xts_status_worst(xts_status_worst(m[0].status, m[1].status), xts_status_worst(ta->status, tb->status));
  for (int o = 0; o < 2; o++) {
    const sc_class_t *first = &sc_classes[o ? cb : ca], *second = &sc_classes[o ? ca : cb];
    double ov, eo;
    const char *why;
    int st = xts_status_worst(m[o].status, xts_status_worst(ta->status, tb->status));
    const char *v = classify(m[o].cyc, m[o].err, o ? tb : ta, o ? ta : tb, st, &ov, &eo, &why);
    char buf[2048];
    xts_json_t j;
    xj_init(&j, buf, sizeof buf);
    xj_obj(&j);
    xj_kv_str(&j, "order", o ? "B>A" : "A>B");
    xj_key(&j, "classes");
    xj_arr(&j);
    xj_str(&j, first->name);
    xj_str(&j, second->name);
    xj_end(&j);
    xj_kv_str(&j, "first", xts_isa[first->insn].mnem);
    xj_kv_str(&j, "second", xts_isa[second->insn].mnem);
    xj_kv_double(&j, "t_first", (o ? tb : ta)->cyc);
    xj_kv_double(&j, "t_second", (o ? ta : tb)->cyc);
    xj_kv_double(&j, "serial", ta->cyc + tb->cyc);
    xj_kv_double(&j, "parallel", fmax(ta->cyc, tb->cyc));
    xj_kv_double(&j, "overlap", ov);
    xj_kv_double(&j, "overlap_err", eo);
    xj_kv_str(&j, "verdict", v);
    xj_kv_str(&j, "coissue", v);
    if (why) xj_kv_str(&j, "why", why);
    xj_kv_bool(&j, "filler_control_ok", ctrl_ok);
    xj_kv_double(&j, "minus_filler", m[o].cyc - 2 * ctrl.cyc);   /* same-shape differential against F F */
    if (m[0].ok && m[1].ok && all_st == XTS_ST_PASS) {
      int dep = fabs(d) > 2 * de && fabs(d) > 0.1;
      xj_kv_bool(&j, "order_dependent", dep);
      xj_kv_double(&j, "ab_minus_ba", d);
      if (dep) xj_kv_str(&j, "faster_order", d < 0 ? "A>B" : "B>A");
    } else xj_kv_str(&j, "order_dependent", "UNDETERMINED");
    if (ca == cb && m[o].ok) {
      double dd = m[o].cyc - 2 * ta->cyc, ee = m[o].err + 2 * ta->err;
      xj_kv_bool(&j, "diag_ok", fabs(dd) <= 2 * ee + 0.05);
      xj_kv_double(&j, "diag_minus_alone", dd);
      if (st == XTS_ST_PASS && fabs(dd) > 2 * ee + 0.05) st = XTS_ST_FAIL;
    }
    {   /* implicit state shared by the members (CR via record forms, CA, LR/CTR, ...) is a real dependence here */
      const xts_insn_t *da = &xts_isa[first->insn], *db = &xts_isa[second->insn];
      unsigned wa = da->implicit_w | (first->flags & XTS_RC ? XTS_IMP_CR : 0), ra = da->implicit_r;
      unsigned wb = db->implicit_w | (second->flags & XTS_RC ? XTS_IMP_CR : 0), rb = db->implicit_r;
      unsigned shared = ((wa & rb) | (wb & ra) | (wa & wb)) & ~(unsigned)XTS_IMP_MEM;
      if (shared) xj_kv_hex(&j, "implicit_dependence", shared, 3);
    }
    if (lazy) xj_kv_bool(&j, "alone_measured_here", 1);
    xj_finish(&j);
    xts_time_emit_ex(ctx, &m[o].s, &m[o].r, -1, &(xts_temit_t){ .metric = "cycles", .extra_json = buf, .status = st });
  }
  return 0;
}

static int ip_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (sc_classes_init() <= 0) return -1;
  switch ((int)c->arg[0]) {
  case 0: return run_control(ctx, 1);
  case 1: return run_alone(ctx, (int)c->arg[1]);
  case 2: return run_pair(ctx, (int)c->arg[1], (int)c->arg[2]);
  }
  return -1;
}

XTS_FAMILY(issue_pair, .kind = "sched", .axes = "sibling", .enumerate = ip_enum, .run = ip_run, .block_cases = 32,
           .desc = "ordered dual-issue pairs of instruction classes vs a verified 1-cycle independent filler");
