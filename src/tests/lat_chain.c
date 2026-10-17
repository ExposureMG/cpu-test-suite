/* lat.chain - dependent-chain latency of every timable register-to-register instruction, per source operand slot.
 *
 * Timable = user, hardware status "executes", not a memory/branch op, destination and at least one source of the same
 * register class (GPR, FPR, VR incl. VMX128, CR bit, CR field). One case per flag variant (base, Rc, OE, OE+Rc) and per
 * source slot:
 *   chain     the slot's register IS the destination (add r3,r3,r4): latency through that operand
 *   rotation  instructions whose destination is also read (VMX128 tied VD/VS, rlwimi, rldimi): two registers alternate
 *             (write r[(i+1)%2], slot reads r[i%2]) so only the slot is on the critical path
 *   acc       the tied destination alone (vmaddfp128 v64,v65,v66: VD = VA*VB + VD with VA, VB fixed)
 * Data is neutral and stays in the normal class for the whole run (GPR 1; FP chain 0.75 and others 1.0; vector splat
 * 0.75f / 1.0f; per-op exceptions in timing_ops.c, noted in detail.note). Method v2: 6 depths 8..64, 5 shuffled
 * repeats, A/B/A, least-squares fit, quality gate. If a chain measures under 1.5 cycles per instruction the front end
 * could be the bound, so a same-shape addi filler is measured and the result flagged when it does not discriminate.
 * Soft baseline: JSON timing.latency for the base variant through the first slot (status regressed if > 10 % off).
 * Metric "latency" (cycles); detail: variant, slot, kind, class, chain registers/values, fit (+ filler). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "timing_ops.h"

static int lc_enum(xts_caselist_t *cl)
{
  for (int id = 0; id < xts_isa_count; id++) {
    tlat_shape_t s;
    if (tlat_shape(id, &s, NULL) < 0) continue;
    unsigned var[4];
    int nv = tlat_variants(id, var);
    for (int v = 0; v < nv; v++) {
      char vn[40];
      tlat_variant_name(id, var[v], vn, sizeof vn);
      int nslot = s.nsrc + (s.tied ? 1 : 0);
      for (int k = 0; k < nslot; k++) {
        int slot = k < s.nsrc ? k : TLAT_SLOT_ACC;
        xts_case_t *c = xts_case_addf(cl, "%s:%s", vn, tlat_slot_name(&s, slot));
        if (!c) return -1;
        c->insn = id;
        c->flags = var[v];
        c->arg[0] = slot;
      }
    }
  }
  return 0;
}

static void put_val(xts_json_t *j, const char *key, const tlat_kern_t *k, int cls, int reg)
{
  for (int i = 0; i < k->nset; i++) {
    if (k->set[i].cls != cls || k->set[i].reg != reg) continue;
    if (cls == TLAT_VR) {
      char b[40];
      const xts_vr_t *v = &k->set[i].val.v;
      snprintf(b, sizeof b, "%08X%08X%08X%08X", v->w[0], v->w[1], v->w[2], v->w[3]);
      xj_kv_str(j, key, b);
    } else {
      xj_kv_hex(j, key, k->set[i].val.u, 16);
    }
    return;
  }
}

/* detail members of one lat.chain record (the fit itself is the emitter's "fit") */
typedef struct lc_det {
  const tlat_kern_t *k, *kf;
  const tlat_shape_t *s;
  const xts_case_t *c;
  const char *variant, *error;
  int slot, filler;                 /* filler: 1 if the front-end check ran */
  const xts_tspec_t *sf;
  const xts_tresult_t *rf;
  double base;
  int disc, has_base;
} lc_det_t;

static void lc_detail(xts_json_t *j, void *arg)
{
  const lc_det_t *d = arg;
  if (d->error) {
    xj_kv_str(j, "error", d->error);
    return;
  }
  const tlat_kern_t *k = d->k;
  const tlat_shape_t *s = d->s;
  xj_kv_str(j, "variant", d->variant);
  xj_kv_str(j, "slot", tlat_slot_name(s, d->slot));
  xj_kv_str(j, "kind", k->shape);
  xj_kv_str(j, "class", tlat_class_name(s->cls));
  xj_kv_int(j, "chain_reg", k->chain_reg[0]);
  if (k->chain_reg[1] >= 0) xj_kv_int(j, "chain_reg2", k->chain_reg[1]);
  put_val(j, "chain_value", k, s->cls, k->chain_reg[0]);
  const xts_insn_t *in = &xts_isa[d->c->insn];
  xj_key(j, "others");
  xj_obj(j);
  for (int i = 0; i < s->nsrc; i++)
    if (i != d->slot) put_val(j, in->ops[s->src[i]].name, k, s->cls, k->op_reg[s->src[i]]);
  xj_end(j);
  const char *note = tlat_neutral_note(s, d->slot);
  if (note) xj_kv_str(j, "note", note);
  if (d->filler) {
    xts_time_fit_json(j, "filler", d->sf, d->rf, d->kf->per_unit);
    xj_kv_bool(j, "discriminates", d->disc);
    if (!d->disc) xj_kv_str(j, "why", "front-end bound: chain not slower than a 1-cycle independent filler");
  }
  if (d->has_base) xj_kv_double(j, "ref_latency", d->base);    /* first-slot base-variant figure, informational */
}

static int lc_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static tlat_kern_t k, kf;
  tlat_shape_t s;
  const char *why = "";
  int slot = (int)c->arg[0];
  lc_det_t d = { .k = &k, .kf = &kf, .s = &s, .c = c, .slot = slot };
  xts_temit_t e = { .metric = "latency", .detail_fn = lc_detail, .detail_arg = &d };
  if (tlat_shape(c->insn, &s, &why) < 0 || tlat_build_chain(&s, c->flags, slot, &k) < 0) {
    d.error = k.err ? k.errmsg : why;
    e.status = XTS_ST_FAIL;
    xts_time_emit_value(ctx, NULL, NULL, c->insn, NAN, &e);
    return 0;
  }
  char vn[40];
  tlat_variant_name(c->insn, c->flags, vn, sizeof vn);
  d.variant = vn;
  xts_tspec_t sp, sf;
  xts_tresult_t r, rf;
  tlat_spec(&sp, &k, tlat_est_latency(c->insn, 4.0), vn);
  xts_time_measure(&sp, ctx, &r);                    /* a failure is recorded by the emitter (r.failed) */
  double v = xts_time_cycles(&r, k.per_unit);
  if (!isnan(v) && v < 1.5 && tlat_build_filler(8, 1, &kf) == 0) {   /* could be front-end bound: same-shape filler */
    tlat_spec(&sf, &kf, 1.0, "addi filler");
    if (xts_time_measure(&sf, ctx, &rf) == 0) {
      d.filler = 1;
      d.sf = &sf;
      d.rf = &rf;
      d.disc = tlat_discriminates(v, xts_time_cycles(&rf, kf.per_unit));
      if (!d.disc) e.status = XTS_ST_INFO;
    }
  }
  int base = c->flags == 0 && slot == 0;             /* JSON timing.latency of the base variant through the first slot */
  if (!base) d.has_base = xts_isa_baseline(c->insn, "latency", &d.base);
  e.per_unit = k.per_unit;
  e.word = tlat_word0(&k);
  e.baseline_key = base ? "latency" : NULL;
  xts_time_emit_ex(ctx, &sp, &r, c->insn, &e);
  return 0;
}

XTS_FAMILY(lat_chain, .kind = "timing", .axes = "code_align,unroll,sibling,other_cores,thread_prio", .enumerate = lc_enum,
           .run = lc_run, .block_cases = 16,
           .desc = "dependent-chain latency of every timable register op, per source slot and flag variant");
