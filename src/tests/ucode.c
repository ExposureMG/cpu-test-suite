/* ucode - cracked / microcoded instruction detection for every instruction the generic planner can time.
 *
 * Per instruction (and its record / overflow variants for integer ops and vector compares):
 *   issue_cost  X alone, independent (rotating destinations, constant sources): cycles per X
 *   extra       U = what X adds to a stream of independent 1-cycle fillers (li rF,imm) that is long enough to cover X's
 *               own busy time (n = T(X)+4 fillers per X). Same-shape differential: kernel X;F*n against F;F*n, so
 *               U = diff + T(F). U ~ 0: X issues in the filler's spare slot; ~ 1: X takes one issue cycle (e.g. shares
 *               the filler's pipe); >= ~2: X holds dispatch for several cycles (microcoded / cracked).
 * Flags in detail: microcoded (yes/no/UNDETERMINED), neighbour (hidden / one_slot / blocks / partial), unpipelined
 * (issue cost >= 2.5 but U < 1.25: the unit is busy, the pipeline is not), blocks_dual_issue (U >= 0.75).
 * Instructions the planner refuses (privileged, illegal, register branches, string ops, ...) get one skip record with the
 * reason, so the coverage is visible. Sibling-thread effects come from the profile's sibling axis (.axes = "sibling"). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sched_classes.h"

typedef struct { uint64_t key; int valid; double cyc, err; int status; } uq_cache_t;
static uq_cache_t ctrl;

static int uq_enum(xts_caselist_t *cl)
{
  xts_case_t *c = xts_case_add(cl, "control.filler");
  if (!c) return -1;
  c->arg[0] = 0;
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->priv != XTS_PRIV_USER || d->hw == XTS_HW_ILLEGAL) continue;
    unsigned vars[3] = { 0, 0, 0 };
    int nv = 1;
    int vcmp = !strcmp(d->form_name, "VC") || !strcmp(d->form_name, "VX128_R");
    if ((d->flags & XTS_RC) && (d->unit == XTS_UNIT_INTEGER || vcmp)) vars[nv++] = XTS_RC;
    if ((d->flags & XTS_OE) && d->unit == XTS_UNIT_INTEGER) vars[nv++] = XTS_OE;
    for (int v = 0; v < nv; v++) {
      if (!(c = xts_case_addf(cl, "%s%s", d->mnem, vars[v] == XTS_RC ? ".rc" : vars[v] == XTS_OE ? ".oe" : ""))) return -1;
      c->insn = id;
      c->flags = vars[v];
      c->arg[0] = 1;
    }
  }
  return 0;
}

static const uq_cache_t *get_ctrl(xts_ctx_t *ctx, sc_meas_t *mout)
{
  uint64_t key = sc_cond_key(ctx);
  if (!mout && ctrl.valid && ctrl.key == key) return &ctrl;
  static sc_meas_t m;
  sc_meas_t *mm = mout ? mout : &m;
  sc_unit_t u;
  sc_unit_init(&u);
  sc_unit_add(&u, SC_FILLER, 0, 1);
  sc_measure(ctx, &u, 1, mm);
  ctrl.key = key;
  ctrl.valid = 1;
  ctrl.cyc = mm->cyc;
  ctrl.err = mm->err;
  ctrl.status = mm->status;                              /* FAIL when the measurement failed */
  return &ctrl;
}

static int run_control(xts_ctx_t *ctx)
{
  static sc_meas_t m;
  const uq_cache_t *c = get_ctrl(ctx, &m);
  int st = c->status;
  int ok = m.ok && fabs(c->cyc - 1.0) <= 0.05;
  if (xts_status_measured(st) && !ok) st = xts_env_loaded() ? XTS_ST_INFO : XTS_ST_FAIL;   /* expected under load */
  char buf[600];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "test", "li rF,imm stream, 1 word per unit");
  xj_kv_double(&j, "expected", 1.0);
  xj_kv_str(&j, "verdict", !m.ok ? "UNDETERMINED" : ok ? "1-cycle filler confirmed" : "FILLER NOT 1/CYCLE");
  xj_kv_double(&j, "err", m.err);
  xj_finish(&j);
  xts_time_emit_ex(ctx, &m.s, &m.r, XI_addi, &(xts_temit_t){ .metric = "issue_cost", .extra_json = buf, .status = st });
  return 0;
}

static int run_insn(const xts_case_t *c, xts_ctx_t *ctx)
{
  static sc_class_t X;
  static sc_meas_t mt, ma, mb;
  const xts_insn_t *d = &xts_isa[c->insn];
  memset(&X, 0, sizeof X);
  X.crimp = -1;
  X.name = NULL;
  char buf[1400];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "unit", xts_unit_name(d->unit));
  xj_kv_str(&j, "variant", c->flags == XTS_RC ? "rc" : c->flags == XTS_OE ? "oe" : "base");
  if (sc_class_adhoc(&X, c->insn, c->flags, NULL) < 0) {
    xj_kv_str(&j, "why", X.plan.why ? X.plan.why : "cannot plan");
    xj_kv_str(&j, "verdict", "UNDETERMINED");
    xj_finish(&j);
    xts_time_emit_value(ctx, NULL, NULL, -1, NAN, &(xts_temit_t){ .metric = "extra", .extra_json = buf, .status = XTS_ST_SKIP });
    return 0;
  }
  const uq_cache_t *F = get_ctrl(ctx, NULL);
  /* 1. isolated issue cost */
  sc_unit_t u;
  sc_unit_init(&u);
  sc_item_t *it = sc_unit_add(&u, -1, 0, 1);
  it->adhoc = &X;
  double est = sc_class_est(&X);
  sc_measure(ctx, &u, est, &mt);
  double T = mt.cyc;
  int st1 = mt.status;
  /* 2. neighbour impact: X ; F*n  vs  F ; F*n */
  double tn = mt.ok ? T : est;
  int n = (int)ceil(tn) + 4;
  if (n < 8) n = 8;
  if (n > 120) n = 120;
  sc_unit_init(&u);
  it = sc_unit_add(&u, -1, 0, 1);
  it->adhoc = &X;
  sc_unit_add(&u, SC_FILLER, 0, n);
  sc_measure(ctx, &u, n + fmax(1, 0.5 * tn), &ma);
  sc_unit_init(&u);
  sc_unit_add(&u, SC_FILLER, 0, n + 1);
  sc_measure(ctx, &u, n + 1, &mb);
  double diff = ma.cyc - mb.cyc, U = diff + F->cyc;
  double eu = sqrt(ma.err * ma.err + mb.err * mb.err) + F->err;
  int st2 = xts_status_worst(xts_status_worst(ma.status, mb.status), F->status);
  /* record 1: issue cost (the ISA baseline is for the base form only) */
  {
    char b1[400];
    xts_json_t j1;
    xj_init(&j1, b1, sizeof b1);
    xj_obj(&j1);
    xj_kv_str(&j1, "shape", "one X per unit, independent");
    xj_kv_double(&j1, "err", mt.err);
    xj_finish(&j1);
    xts_time_emit_ex(ctx, &mt.s, &mt.r, c->insn, &(xts_temit_t){ .metric = "issue_cost", .baseline_key = c->flags ? NULL : "*",
                                                                 .extra_json = b1 });
  }
  /* record 2: neighbour impact + flags */
  int clean = xts_status_measured(st2) && st1 != XTS_ST_SKIP;
  const char *mc = "UNDETERMINED", *nb = "UNDETERMINED";
  if (clean && eu <= 0.3) {
    if (U - 2 * eu >= 1.75) mc = "yes";
    else if (U + 2 * eu < 1.25) mc = "no";
    if (U + 2 * eu <= 0.3) nb = "hidden";
    else if (U - 2 * eu >= 0.7 && U + 2 * eu <= 1.3) nb = "one_slot";
    else if (U - 2 * eu >= 1.75) nb = "blocks";
    else if (U - 2 * eu > 0.3 && U + 2 * eu < 1.75) nb = "partial";
  }
  xj_kv_int(&j, "fillers", n);
  xj_kv_double(&j, "filler_cost", F->cyc);
  xj_kv_double(&j, "issue_cost", T);
  xj_kv_double(&j, "diff_vs_filler", diff);
  xj_kv_double(&j, "extra_err", eu);
  xj_kv_str(&j, "microcoded", mc);
  xj_kv_str(&j, "neighbour", nb);
  if (clean && xts_status_measured(st1)) {
    xj_kv_bool(&j, "unpipelined", T >= 2.5 && U < 1.25);
    xj_kv_bool(&j, "blocks_dual_issue", U - 2 * eu >= 0.75);
  }
  xj_kv_str(&j, "verdict", mc);
  if (!clean) xj_kv_str(&j, "why", st2 == XTS_ST_SKIP ? "not executed (host)" : "a measurement did not pass the quality gate");
  else if (eu > 0.3) xj_kv_str(&j, "why", "error bar > 0.3 cycles");
  int st = st2;
  if (xts_status_measured(st) && !strcmp(mc, "UNDETERMINED")) st = XTS_ST_NOISY;
  xj_finish(&j);
  /* "fit" = X ; F*n, "ref" = F*(n+1); U = a - b + T(F) */
  xts_time_emit_diff(ctx, &ma.s, &ma.r, &mb.s, &mb.r, c->insn, &(xts_temit_t){ .metric = "extra", XTS_TE_VALUE(U),
                                                                               .extra_json = buf, .status = st });
  return 0;
}

static int uq_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (c->arg[0] == 0) return run_control(ctx);
  return run_insn(c, ctx);
}

XTS_FAMILY(ucode, .kind = "sched", .axes = "sibling", .enumerate = uq_enum, .run = uq_run, .block_cases = 16,
           .desc = "microcoded/cracked detection: isolated issue cost and dispatch cycles taken from a 1-cycle filler stream");
