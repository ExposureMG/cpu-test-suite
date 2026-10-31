/* issue.parity - does dual issue depend on whether a pair starts on an even or an odd word?
 *
 * issue.pair measures "A B" units in the line layout (loop top at byte 12 of a 16-byte fetch group), so every unit's A
 * sits on an odd word and its B on the following even word. If the PPE pairs instructions at fixed doubleword
 * boundaries, the pairs the hardware actually forms there are (B, A), and the matrix's order asymmetries (lwz;X 1.35 vs
 * X;lwz 2.04-2.56, X;stw/lfd/stvx 2.04 vs stw/lfd/stvx;X ~3, vfp;vlogic 3.12 vs vlogic;vfp 2.15) would be a parity effect
 * that a compiler can only use if it knows the word parity of its instructions.
 *
 *   control.filler  li rF,imm stream at both parities: must cost the same (one word per cycle, parity-free); otherwise
 *                   the parity verdicts below are UNDETERMINED.
 *   pair.<A>+<B>    the issue.pair classes (SC_U_PAIR), every unordered pair: A B per unit at the odd start (phase_pad 0,
 *                   exactly issue.pair's kernel) and at the even start (phase_pad 1: one filler before the body and three
 *                   after it, a constant 4 words per iteration that the depth fit removes), for both orders. One record
 *                   per order: value = cycles per pair at the EVEN start (the hardware doubleword pair is A,B), detail
 *                   cycles_odd, even_minus_odd and its error bar, parity_dependent (|difference| > 2 sigma and > 0.1),
 *                   and the faster parity.
 * Default condition only (no axes): the question is one of the front end's issue rule, not of a load condition. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sched_classes.h"

#define MAXC 64
static int ctrl_ok;
static uint64_t ctrl_key;
static int ctrl_valid;

static int ipp_enum(xts_caselist_t *cl)
{
  sc_classes_init();
  int idx[MAXC], n = 0;
  for (int i = 0; i < sc_nclasses && n < MAXC; i++)
    if (sc_class_ok(i) && (sc_classes[i].use & SC_U_PAIR)) idx[n++] = i;
  xts_case_t *c = xts_case_add(cl, "control.filler");
  if (!c) return -1;
  c->arg[0] = 0;
  for (int i = 0; i < n; i++)
    for (int k = i; k < n; k++) {
      if (!(c = xts_case_addf(cl, "pair.%s+%s", sc_classes[idx[i]].name, sc_classes[idx[k]].name))) return -1;
      c->arg[0] = 1;
      c->arg[1] = idx[i];
      c->arg[2] = idx[k];
    }
  return 0;
}

/* one unit: classes a then b (b < 0: two fillers), at the given phase pad */
static void measure_unit(xts_ctx_t *ctx, int a, int b, int pad, double est, sc_meas_t *m)
{
  sc_unit_t u;
  sc_unit_init(&u);
  sc_unit_add(&u, a, 0, b < 0 ? 2 : 1);
  if (b >= 0) sc_unit_add(&u, b, 1, 1);
  u.phase_pad = pad;
  sc_measure(ctx, &u, est, m);
}

static int run_control(xts_ctx_t *ctx, int emit)
{
  static sc_meas_t m[2];
  for (int p = 0; p < 2; p++) measure_unit(ctx, SC_FILLER, -1, p, 2, &m[p]);
  int st = xts_status_worst(m[0].status, m[1].status);
  double d = m[1].cyc - m[0].cyc, de = sqrt(m[0].err * m[0].err + m[1].err * m[1].err);
  int ok = m[0].ok && m[1].ok && fabs(m[0].cyc / 2 - 1.0) <= 0.05 && fabs(d) <= 2 * de + 0.05;
  ctrl_ok = ok;
  ctrl_key = sc_cond_key(ctx);
  ctrl_valid = 1;
  if (!emit) return 0;
  char buf[2400];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "test", "li rF,imm stream, 2 words per unit, odd start (phase_pad 0) vs even start (phase_pad 1)");
  xj_kv_double(&j, "per_word_odd", m[0].cyc / 2);
  xj_kv_double(&j, "per_word_even", m[1].cyc / 2);
  xj_kv_double(&j, "even_minus_odd", d);
  xj_kv_double(&j, "err", de);
  xj_kv_str(&j, "verdict", m[0].ok && m[1].ok ? (ok ? "parity-free 1-cycle filler" : "FILLER DEPENDS ON PARITY")
                                              : "UNDETERMINED");
  xts_time_fit_json(&j, "fit_even", &m[1].s, &m[1].r, 2);
  xj_finish(&j);
  if (xts_status_measured(st) && !ok) st = XTS_ST_FAIL;
  xts_time_emit_ex(ctx, &m[0].s, &m[0].r, XI_addi, &(xts_temit_t){ .metric = "issue_cost", .per_unit = 2,
                                                                   .extra_json = buf, .status = st });
  return 0;
}

static int run_pair(xts_ctx_t *ctx, int ca, int cb)
{
  static sc_meas_t m[2][2];                               /* [order][parity: 0 odd, 1 even] */
  if (!(ctrl_valid && ctrl_key == sc_cond_key(ctx))) run_control(ctx, 0);
  double est = sc_class_est(&sc_classes[ca]) + sc_class_est(&sc_classes[cb]);
  for (int o = 0; o < 2; o++)
    for (int p = 0; p < 2; p++) measure_unit(ctx, o ? cb : ca, o ? ca : cb, p, est, &m[o][p]);
  for (int o = 0; o < 2; o++) {
    const sc_class_t *first = &sc_classes[o ? cb : ca], *second = &sc_classes[o ? ca : cb];
    const sc_meas_t *odd = &m[o][0], *even = &m[o][1];
    int st = xts_status_worst(odd->status, even->status);
    double d = even->cyc - odd->cyc, de = sqrt(odd->err * odd->err + even->err * even->err);
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
    xj_kv_str(&j, "parity", "value: first on an even word (doubleword pair = first,second); cycles_odd: first on an odd "
                            "word (issue.pair's layout)");
    xj_kv_double(&j, "cycles_even", even->cyc);
    xj_kv_double(&j, "cycles_odd", odd->cyc);
    xj_kv_double(&j, "even_minus_odd", d);
    xj_kv_double(&j, "err", de);
    xj_kv_bool(&j, "filler_control_ok", ctrl_ok);
    if (even->ok && odd->ok && xts_status_measured(st) && ctrl_ok) {
      int dep = fabs(d) > 2 * de && fabs(d) > 0.1;
      xj_kv_bool(&j, "parity_dependent", dep);
      if (dep) xj_kv_str(&j, "faster_parity", d < 0 ? "even" : "odd");
    } else xj_kv_str(&j, "parity_dependent", "UNDETERMINED");
    xts_time_fit_json(&j, "fit_odd", &odd->s, &odd->r, 1);
    xj_finish(&j);
    xts_time_emit_ex(ctx, &even->s, &even->r, -1, &(xts_temit_t){ .metric = "cycles", .extra_json = buf, .status = st });
  }
  return 0;
}

static int ipp_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (sc_classes_init() <= 0) return -1;
  switch ((int)c->arg[0]) {
  case 0: return run_control(ctx, 1);
  case 1: return run_pair(ctx, (int)c->arg[1], (int)c->arg[2]);
  }
  return -1;
}

XTS_FAMILY(issue_parity, .kind = "sched", .enumerate = ipp_enum, .run = ipp_run, .block_cases = 32,
           .desc = "dual issue by word parity: issue.pair's A B units at an odd and an even start");
