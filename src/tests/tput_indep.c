/* tput.indep - reciprocal throughput from N independent chains, N = 1 2 3 4 6 8 12 16.
 *
 * Every timable instruction of lat.chain (same shape analysis) and flag variant. Chain c feeds itself through the first
 * source slot (tied VMX128 ops and rlwimi/rldimi: through the accumulator), the other sources are shared fixed
 * registers with the neutral lat.chain data. A unit of the timing engine is one round of N instructions, so cycles per
 * instruction = cycles per unit / N. The N curve: N = 1 is the latency, the cost falls as ~latency/N until the issue
 * interval is reached; an unpipelined unit shows a flat curve. Every N is also measured against a same-shape filler
 * (N rotating `addi r,K,1`): when the instruction is not slower than that 1-cycle independent filler the point is front-end
 * bound and cannot discriminate (flagged, the value is then only an upper bound).
 * Registers: GPR pool r3-r12 r14-r26 r31 (never r0 r1 r2 r13 r27-r30), FPR f0-f31, VR v0-v31 or v64-v127 (VMX128),
 * CR bits 0-31, CR fields 0-7 (N > 8 skipped for mcrf).
 * Records per case:
 *   rthroughput  value = best cycles per instruction over N (saturation); n/min/median/mad/fit and detail.fit are those
 *                of the best point (n = its runs); detail.curve = every N, n_sat, latency_n1, pipelined, points_executed;
 *                status info if the saturated point is front-end bound; baseline JSON "issue" (base variant)
 *   depth        value = latency_n1 / rthroughput (instructions in flight at saturation), detail.n_sat
 * Axes: unroll (depth list), chains (measure only that N; then latency_n1/depth need N = 1), SMT axes, code_align. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "timing_ops.h"
#include <xts/cond_util.h>

static const int tp_n[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
#define TP_NN ((int)(sizeof tp_n / sizeof tp_n[0]))

static int tp_enum(xts_caselist_t *cl)
{
  for (int id = 0; id < xts_isa_count; id++) {
    tlat_shape_t s;
    if (tlat_shape(id, &s, NULL) < 0) continue;
    unsigned var[4];
    int nv = tlat_variants(id, var);
    for (int v = 0; v < nv; v++) {
      char vn[40];
      tlat_variant_name(id, var[v], vn, sizeof vn);
      xts_case_t *c = xts_case_add(cl, vn);
      if (!c) return -1;
      c->insn = id;
      c->flags = var[v];
    }
  }
  return 0;
}

/* one point of the N curve */
typedef struct tp_pt {
  int n, built, done;               /* built: kernel exists; done: measured without a build/exec error */
  char err[96];                     /* why not built / not measured */
  xts_tspec_t sp, sf;
  xts_tresult_t r, rf;
  int per_unit, has_fil, disc;
  double cyc, fil;
} tp_pt_t;

typedef struct tp_det {
  const tp_pt_t *pt;                /* [TP_NN] */
  const char *variant, *cls, *slot, *note, *why;
  int best, nsat, nexec;
  double lat1;
  double ref_issue;                 /* informational baseline of a non-base variant */
  int has_ref;
} tp_det_t;

static void tp_detail(xts_json_t *j, void *arg)
{
  const tp_det_t *d = arg;
  xj_kv_str(j, "variant", d->variant);
  xj_kv_str(j, "class", d->cls);
  xj_kv_str(j, "slot", d->slot);
  if (d->note) xj_kv_str(j, "note", d->note);
  xj_key(j, "curve");
  xj_arr(j);
  for (int i = 0; i < TP_NN; i++) {
    const tp_pt_t *p = &d->pt[i];
    if (!p->n) continue;                         /* not selected by the chains axis */
    xj_obj(j);
    xj_kv_int(j, "n", p->n);
    if (!p->built) xj_kv_str(j, "skipped", p->err);
    else if (!p->done) xj_kv_str(j, "error", p->err);
    else {
      xj_kv_double(j, "cycles", p->cyc);
      xj_kv_str(j, "status", xts_status_name(p->r.status));
      if (p->r.why[0]) xj_kv_str(j, "why", p->r.why);
      if (p->r.ok) {
        xj_kv_double(j, "r2", p->r.fit.r2);
        xj_kv_double(j, "spread", p->r.spread_max);
      }
      if (p->has_fil) {
        xj_kv_double(j, "filler", p->fil);
        if (p->rf.ok) xj_kv_bool(j, "discriminates", p->disc);
      }
    }
    xj_end(j);
  }
  xj_end(j);
  if (d->best >= 0) {
    xj_kv_int(j, "n_best", d->pt[d->best].n);
    xj_kv_int(j, "n_sat", d->nsat >= 0 ? d->pt[d->nsat].n : -1);
    xj_kv_double(j, "latency_n1", d->lat1);
    xj_kv_bool(j, "pipelined", !isnan(d->lat1) && d->lat1 / d->pt[d->best].cyc >= 1.3);
    xj_kv_double(j, "filler_at_best", d->pt[d->best].fil);
  }
  if (d->why) xj_kv_str(j, "why", d->why);
  if (d->has_ref) xj_kv_double(j, "ref_issue", d->ref_issue);
  xj_kv_int(j, "points_executed", d->nexec);
}

static void tp_depth_detail(xts_json_t *j, void *arg)
{
  const tp_det_t *d = arg;
  xj_kv_str(j, "variant", d->variant);
  xj_kv_str(j, "meaning", "latency_n1 / rthroughput: independent instructions in flight at saturation");
  xj_kv_double(j, "latency_n1", d->lat1);
  xj_kv_double(j, "rthroughput", d->best >= 0 ? d->pt[d->best].cyc : NAN);
  xj_kv_int(j, "n_sat", d->nsat >= 0 ? d->pt[d->nsat].n : -1);
}

static int tp_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static tlat_kern_t k, kf;
  static tp_pt_t pt[TP_NN];
  tlat_shape_t s;
  const char *why = "";
  char vn[40];
  tlat_variant_name(c->insn, c->flags, vn, sizeof vn);
  if (tlat_shape(c->insn, &s, &why) < 0) {
    tp_det_t d = { .pt = pt, .variant = vn, .cls = "", .slot = "", .best = -1, .nsat = -1 };
    xts_temit_t e = { .metric = "rthroughput", .status = XTS_ST_FAIL, .detail_fn = tp_detail, .detail_arg = &d };
    d.why = why;
    xts_time_emit_value(ctx, NULL, NULL, c->insn, NAN, &e);
    return 0;
  }
  double lat_est = tlat_est_latency(c->insn, 4.0);
  const xts_insn_t *isa = &xts_isa[c->insn];
  double iss_est = isa->issue > 0 ? isa->issue : 1.0;
  int slotidx = s.tied ? TLAT_SLOT_ACC : 0;
  uint32_t word = 0;
  int any_err = 0, nexec = 0;
  int only = xts_cond_chains(0);               /* chains axis: measure that N only */
  memset(pt, 0, sizeof pt);
  for (int i = 0; i < TP_NN; i++) {
    tp_pt_t *p = &pt[i];
    int n = tp_n[i];
    p->cyc = p->fil = NAN;
    p->disc = 1;
    if (only > 0 && n != only) continue;
    p->n = n;
    if (tlat_build_nchains(&s, c->flags, n, &k) < 0) {
      snprintf(p->err, sizeof p->err, "%s", k.errmsg);
      continue;
    }
    p->built = 1;
    if (!word) word = tlat_word0(&k);
    double est = n * (lat_est / n > iss_est ? lat_est / n : iss_est);   /* cycles per round */
    tlat_spec(&p->sp, &k, est, vn);
    p->per_unit = k.per_unit;
    if (xts_time_measure(&p->sp, ctx, &p->r) < 0) {
      snprintf(p->err, sizeof p->err, "%s", p->r.why);
      any_err = 1;
      continue;
    }
    p->done = 1;
    p->cyc = xts_time_cycles(&p->r, k.per_unit);
    if (p->r.ok) nexec++;
    if (tlat_build_filler(n < 8 ? 8 : n, n, &kf) == 0) {
      tlat_spec(&p->sf, &kf, (double)n, "addi filler");
      if (xts_time_measure(&p->sf, ctx, &p->rf) == 0) {
        p->has_fil = 1;
        p->fil = xts_time_cycles(&p->rf, kf.per_unit);
        p->disc = tlat_discriminates(p->cyc, p->fil);
      }
    }
  }
  /* saturation: best measured point; n_sat: smallest N within 5 % of it */
  int best = -1, nsat = -1;
  for (int i = 0; i < TP_NN; i++)
    if (pt[i].done && !isnan(pt[i].cyc) && (best < 0 || pt[i].cyc < pt[best].cyc)) best = i;
  for (int i = 0; best >= 0 && i < TP_NN; i++)
    if (pt[i].done && !isnan(pt[i].cyc) && pt[i].cyc <= pt[best].cyc * 1.05) { nsat = i; break; }
  double lat1 = pt[0].done ? pt[0].cyc : NAN;
  tp_det_t d = { .pt = pt, .variant = vn, .cls = tlat_class_name(s.cls), .slot = tlat_slot_name(&s, slotidx),
                 .note = tlat_neutral_note(&s, slotidx), .best = best, .nsat = nsat, .nexec = nexec, .lat1 = lat1 };
  xts_temit_t e = { .metric = "rthroughput", .word = word, .detail_fn = tp_detail, .detail_arg = &d };
  int status, st0 = pt[0].done ? pt[0].r.status : XTS_ST_SKIP;
  if (best >= 0) {
    status = xts_status_worst(pt[best].r.status, pt[nsat].r.status);
    if (!pt[best].disc) {
      d.why = "front-end bound: not slower than a 1-cycle independent filler (value is an upper bound)";
      status = xts_status_worst(status, XTS_ST_INFO);
    }
    if (c->flags == 0) e.baseline_key = "issue";
    else d.has_ref = xts_isa_baseline(c->insn, "issue", &d.ref_issue);
    e.per_unit = pt[best].per_unit;
    e.status = status;
    xts_time_emit_ex(ctx, &pt[best].sp, &pt[best].r, c->insn, &e);
  } else {
    status = any_err ? XTS_ST_FAIL : XTS_ST_SKIP;
    d.why = any_err ? "kernel build failed" : (ctx->host ? "not executed (host dry run)" : "no point measured");
    e.status = status;
    int first = 0;                               /* nothing usable: report the first point that ran (its fit says why) */
    while (first < TP_NN && !pt[first].done) first++;
    if (first < TP_NN) {
      e.per_unit = pt[first].per_unit;
      xts_time_emit_ex(ctx, &pt[first].sp, &pt[first].r, c->insn, &e);
    } else {
      xts_time_emit_value(ctx, NULL, NULL, c->insn, NAN, &e);
    }
  }

  /* depth: instructions in flight at saturation */
  double v = best >= 0 ? pt[best].cyc : NAN;
  xts_temit_t e2 = { .metric = "depth", .unit = "count", .word = word, .detail_fn = tp_depth_detail, .detail_arg = &d };
  e2.status = best < 0 ? status : xts_status_worst(status, st0);   /* status: the rthroughput record's, before its baseline check */
  xts_time_emit_value(ctx, NULL, NULL, c->insn, (!isnan(lat1) && v > 0) ? lat1 / v : NAN, &e2);
  return 0;
}

XTS_FAMILY(tput_indep, .kind = "timing", .axes = "code_align,unroll,chains,sibling,other_cores,thread_prio", .enumerate = tp_enum,
           .run = tp_run, .block_cases = 8,
           .desc = "reciprocal throughput from 1..16 independent chains (N curve), same-shape filler check");
