/* bypass.matrix - producer class x consumer class x consumer operand slot: how many cycles does the consumer stall when it
 * directly follows its producer, and how much of that is more than the producer's own (same-file) latency?
 *
 * Same-shape twin kernels per case, 2 instructions per depth unit (plus `d` fillers for distance sweeps):
 *   dep:  P_j ; [li x d] ; C_j   with C_j's operand <slot> := P_j's destination
 *   ind:  identical words except that operand reads the constant it would read anyway
 * Units are independent of each other (rotating banks), so penalty = dep - ind is the stall per adjacent P->C.
 *   penalty  (metric penalty)  dep - ind
 *   extra    (metric extra)    penalty - penalty(P -> reference consumer of the same file: add RA / fadd FA / vand128 VA /
 *                              bc BI), i.e. what this consumer/slot costs beyond P's ordinary forwarding latency
 * Address operands get producer outputs that ARE the consumer's usual base pointer (add rB,0 / ld of a stored pointer),
 * index operands get 0, so dep and ind touch the same addresses. Integer-valued producers (fctiwz, vcfpsxws) never feed
 * floating-point arithmetic (denormal inputs would be measured instead). CR producers feed bc/crxor/mcrf through the field
 * they write (record forms through CR0/CR6).
 *   dist.*      the same at 0..16 fillers between P and C; the family finish hook adds, per sweep and condition vector, a
 *               metric latency record (case id of the last distance) = fillers needed to hide the stall (filler rate
 *               1/cycle, see issue.pair control.filler), built from the sweep's penalty records on disk
 *   mtctr>bctr / mtlr>blr   register branch d words after the move vs `b` to the same word (penalty per distance; the
 *               CTR form is timed in the GPR loop frame, CTR belongs to the unit) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sched_classes.h"

enum { BK_MATRIX = 0, BK_DIST, BK_SPECIAL };
static const int dists[] = { 0, 1, 2, 4, 8, 16 };
static const int sdists[] = { 0, 4, 8, 16, 24, 32 };
#define ND 6
static const char *const sweeps[][3] = {
  { "int", "lwz", "RA0" }, { "lwz", "int", "RA" }, { "ld", "lwz", "RA0" }, { "lfd", "fp", "FA" }, { "fp", "stfd", "FS" },
  { "lvx", "vlogic", "VA128" }, { "vfp", "stvx", "VS128" }, { "vfp", "vfp", "VA128" }, { "cmp", "br_t", "BI" },
  { "int", "stw", "RS" }, { "mul", "int", "RA" }, { "fp", "fp", "FA" },
};
#define NSW ((int)(sizeof sweeps / sizeof sweeps[0]))
static const char *const refs[5][2] = { { NULL, NULL }, { "int", "RA" }, { "fp", "FA" }, { "vlogic", "VA128" }, { "br_t", "BI" } };

typedef struct { uint64_t key; int valid; double pen, err; int status; } bp_cache_t;
static bp_cache_t refc[64];                 /* reference-consumer penalty per producer (measurement cache) */

/* register file a producer feeds */
static int prod_file(const sc_class_t *p) { return p->crimp >= 0 ? SC_F_CR : p->plan.dest_file; }

/* file of consumer operand i, and whether it is a readable slot */
static int cons_slot(const sc_class_t *c, int i, int *file)
{
  const xts_insn_t *d = &xts_isa[c->insn];
  const sc_plan_t *p = &c->plan;
  int k = p->kind[i];
  if (!(d->ops[i].role & XTS_ROLE_R)) return 0;
  if (k == SC_K_TIED || k == SC_K_UPD) return 0;
  if (k == SC_K_IMM) {
    if (d->ops[i].cls != XTS_OC_CRB) return 0;
    *file = SC_F_CR;
    return 1;
  }
  if (k == SC_K_DISP || k == SC_K_CRM) return 0;
  *file = p->file[i];
  if (*file == SC_F_NONE) return 0;
  if (c->slots) {
    const char *s = strstr(c->slots, d->ops[i].name);
    size_t n = strlen(d->ops[i].name);
    if (!s || (s[n] && s[n] != ',')) return 0;
  }
  return 1;
}

static const char *slot_role(const sc_class_t *c, int i)
{
  switch (c->plan.kind[i]) {
  case SC_K_BASE: return "base";
  case SC_K_INDEX: return "index";
  case SC_K_DATA: return "store-data";
  case SC_K_DEST: return "accumulator";
  case SC_K_CRF_S: case SC_K_CRB_S: case SC_K_IMM: return "cr";
  default: return "source";
  }
}

/* can producer P feed consumer C's operand i? mode out; NULL or the reason it cannot */
static const char *link_ok(const sc_class_t *P, const sc_class_t *C, int i, int *mode)
{
  int file = SC_F_NONE;
  *mode = SC_MODE_DATA;
  if (!cons_slot(C, i, &file) || file != prod_file(P)) return "not a slot of the producer's file";
  if (file == SC_F_CR) { *mode = SC_MODE_IDX; return NULL; }
  if (C->plan.kind[i] == SC_K_BASE) { if (!(P->use & SC_PTR_OK)) return "producer cannot form an address"; *mode = SC_MODE_PTR; }
  if (C->plan.kind[i] == SC_K_INDEX) { if (!(P->use & SC_IDX_OK)) return "producer cannot deliver a zero index"; *mode = SC_MODE_IDX; }
  if ((P->use & SC_INTVAL) && (C->use & SC_FPARITH)) return "integer-valued result into FP arithmetic";
  return NULL;
}

static int bp_enum(xts_caselist_t *cl)
{
  sc_classes_init();
  xts_case_t *c;
  for (int pi = 0; pi < sc_nclasses; pi++) {
    const sc_class_t *P = &sc_classes[pi];
    if (!sc_class_ok(pi) || !(P->use & SC_U_BP)) continue;
    for (int ci = 0; ci < sc_nclasses; ci++) {
      const sc_class_t *C = &sc_classes[ci];
      if (!sc_class_ok(ci) || !(C->use & SC_U_BC)) continue;
      for (int i = 0; i < C->plan.nops; i++) {
        int mode;
        if (link_ok(P, C, i, &mode)) continue;
        if (!(c = xts_case_addf(cl, "%s>%s.%s", P->name, C->name, xts_isa[C->insn].ops[i].name))) return -1;
        c->arg[0] = BK_MATRIX;
        c->arg[1] = pi;
        c->arg[2] = ci;
        c->arg[3] = i;
        c->insn = C->insn;
      }
    }
  }
  for (int s = 0; s < NSW; s++) {
    int pi = sc_class_find(sweeps[s][0]), ci = sc_class_find(sweeps[s][1]);
    if (!sc_class_ok(pi) || !sc_class_ok(ci)) continue;
    int op = xts_op_index(&xts_isa[sc_classes[ci].insn], sweeps[s][2]), mode;
    if (op < 0 || link_ok(&sc_classes[pi], &sc_classes[ci], op, &mode)) continue;
    for (int d = 0; d < ND; d++) {
      if (!(c = xts_case_addf(cl, "dist.%s>%s.%s.d%d", sweeps[s][0], sweeps[s][1], sweeps[s][2], dists[d]))) return -1;
      c->arg[0] = BK_DIST;
      c->arg[1] = pi;
      c->arg[2] = ci;
      c->arg[3] = op | (d << 8);
    }
  }
  for (int sp = 1; sp <= 2; sp++)
    for (int d = 0; d < ND; d++) {
      if (!(c = xts_case_addf(cl, "%s.d%d", sp == 1 ? "mtctr>bctr" : "mtlr>blr", sdists[d]))) return -1;
      c->arg[0] = BK_SPECIAL;
      c->arg[1] = sp;
      c->arg[2] = d;
      c->insn = sp == 1 ? XI_bcctrx : XI_bclrx;
    }
  return 0;
}

static double prod_latency(const sc_class_t *P)
{
  double v;
  if (xts_isa_baseline(P->insn, "latency", &v)) return v;
  if (xts_isa_baseline(P->insn, "load_use_latency", &v)) return v;
  return NAN;
}

/* measure dep and ind for P -> C operand op with d fillers; penalty out */
static int pair_measure(xts_ctx_t *ctx, int pi, int ci, int op, int d, sc_meas_t *dep, sc_meas_t *ind, double *pen,
                        double *err, int *status)
{
  const sc_class_t *P = &sc_classes[pi], *C = &sc_classes[ci];
  int mode;
  link_ok(P, C, op, &mode);
  double lat = prod_latency(P);
  double est = (isnan(lat) ? 10 : lat) + sc_class_est(C) + d;
  for (int v = 0; v < 2; v++) {
    sc_unit_t u;
    sc_unit_init(&u);
    sc_item_t *ip = sc_unit_add(&u, pi, 0, 1);
    ip->mode = mode;
    ip->ptr_region = C->plan.region;
    if (d) sc_unit_add(&u, SC_FILLER, 0, d);
    sc_item_t *ic = sc_unit_add(&u, ci, 1, 1);
    ic->link_from = 0;
    ic->link_op = op;
    ic->link = v == 0 ? SC_LINK_DEP : SC_LINK_IND;
    ic->crsel = P->crbit;
    sc_measure(ctx, &u, est, v == 0 ? dep : ind);
  }
  *pen = dep->cyc - ind->cyc;
  *err = sqrt(dep->err * dep->err + ind->err * ind->err);
  *status = xts_status_worst(dep->status, ind->status);   /* FAIL when either measurement failed */
  return 0;
}

static const char *pen_verdict(double v, double err, int st, const char *pos, const char *zero, const char *neg)
{
  if (!xts_status_measured(st)) return "UNDETERMINED";
  double th = 2 * err + 0.1;
  return v > th ? pos : v < -th ? neg : zero;
}

/* reference consumer/slot for producer pi (same register file), -1 if none */
static int ref_of(int pi, int *ref_ci, int *ref_op)
{
  const sc_class_t *P = &sc_classes[pi];
  int f = prod_file(P);
  *ref_ci = *ref_op = -1;
  if (f < 1 || f > 4) return -1;
  int ci = sc_class_find(refs[f][0]);
  if (!sc_class_ok(ci)) return -1;
  int op = xts_op_index(&xts_isa[sc_classes[ci].insn], refs[f][1]), mode;
  if (op < 0 || link_ok(P, &sc_classes[ci], op, &mode)) return -1;
  *ref_ci = ci;
  *ref_op = op;
  return 0;
}

static int ref_penalty(xts_ctx_t *ctx, int pi, double *pen, double *err, int *status, int *ref_ci, int *ref_op)
{
  if (ref_of(pi, ref_ci, ref_op) < 0) return -1;
  int ci = *ref_ci, op = *ref_op;
  uint64_t key = sc_cond_key(ctx);
  bp_cache_t *rc = &refc[pi & 63];
  if (!(rc->valid && rc->key == key)) {
    static sc_meas_t a, b;
    pair_measure(ctx, pi, ci, op, 0, &a, &b, &rc->pen, &rc->err, &rc->status);
    rc->key = key;
    rc->valid = 1;
  }
  *pen = rc->pen;
  *err = rc->err;
  *status = rc->status;
  return 0;
}

static void common_detail(xts_json_t *j, const sc_class_t *P, const sc_class_t *C, int op, int d, double pen, double err)
{
  xj_kv_str(j, "producer", P->name);
  xj_kv_str(j, "consumer", C->name);
  xj_kv_str(j, "producer_mnem", xts_isa[P->insn].mnem);
  xj_kv_str(j, "consumer_mnem", xts_isa[C->insn].mnem);
  xj_kv_str(j, "slot", xts_isa[C->insn].ops[op].name);
  xj_kv_str(j, "slot_role", slot_role(C, op));
  xj_kv_int(j, "distance", d);
  xj_kv_str(j, "order", "producer>consumer");
  xj_kv_double(j, "penalty", pen);
  xj_kv_double(j, "penalty_err", err);
  xj_kv_double(j, "producer_latency_baseline", prod_latency(P));
}

static int run_pair(xts_ctx_t *ctx, const xts_case_t *c)
{
  int pi = (int)c->arg[1], ci = (int)c->arg[2], op = (int)(c->arg[3] & 0xFF), di = (int)((c->arg[3] >> 8) & 0xFF);
  int d = c->arg[0] == BK_DIST ? dists[di] : 0;
  const sc_class_t *P = &sc_classes[pi], *C = &sc_classes[ci];
  static sc_meas_t dep, ind;
  double pen, err;
  int st;
  pair_measure(ctx, pi, ci, op, d, &dep, &ind, &pen, &err, &st);
  double rpen = NAN, rerr = NAN;
  int rst = XTS_ST_SKIP, rci = -1, rop = -1;
  if (c->arg[0] == BK_MATRIX) {
    if (ref_of(pi, &rci, &rop) == 0 && rci == ci && rop == op) {   /* this case is the reference: reuse it */
      bp_cache_t *rc = &refc[pi & 63];
      rc->key = sc_cond_key(ctx); rc->valid = 1; rc->pen = pen; rc->err = err; rc->status = st;
    }
    ref_penalty(ctx, pi, &rpen, &rerr, &rst, &rci, &rop);
  }
  const char *why = st == XTS_ST_SKIP ? "not executed (host)" : !xts_status_measured(st) ? "dep or ind did not pass the quality gate" : NULL;
  /* penalty record: fit = dep, ref = ind */
  {
    char buf[1000];
    xts_json_t j;
    xj_init(&j, buf, sizeof buf);
    xj_obj(&j);
    common_detail(&j, P, C, op, d, pen, err);
    xj_kv_str(&j, "verdict", pen_verdict(pen, err, st, "stall", "none", "NEGATIVE"));
    if (why) xj_kv_str(&j, "why", why);
    xj_finish(&j);
    xts_time_emit_diff(ctx, &dep.s, &dep.r, &ind.s, &ind.r, C->insn, &(xts_temit_t){ .metric = "penalty", .extra_json = buf });
  }
  if (c->arg[0] == BK_MATRIX) {   /* extra over the reference consumer (the distance sweeps are summarised in finish) */
    double ex = pen - rpen, ee = sqrt(err * err + rerr * rerr);
    int xs = xts_status_worst(st, rst);
    char buf[1000];
    xts_json_t j;
    xj_init(&j, buf, sizeof buf);
    xj_obj(&j);
    common_detail(&j, P, C, op, d, pen, err);
    if (rci >= 0) {
      xj_kv_str(&j, "reference", sc_classes[rci].name);
      xj_kv_str(&j, "reference_slot", xts_isa[sc_classes[rci].insn].ops[rop].name);
      xj_kv_double(&j, "reference_penalty", rpen);
      xj_kv_bool(&j, "is_reference", rci == ci && rop == op);
    } else {
      xj_kv_str(&j, "reference", "none");
      xs = XTS_ST_SKIP;
    }
    xj_kv_double(&j, "extra_err", ee);
    xj_kv_str(&j, "verdict", rci < 0 ? "UNDETERMINED" : pen_verdict(ex, ee, xs, "extra", "none", "less"));
    if (xs == XTS_ST_SKIP && !ctx->host) xj_kv_str(&j, "why", "no reference consumer for this producer");
    else if (why) xj_kv_str(&j, "why", why);
    xj_finish(&j);
    xts_time_emit_value(ctx, &dep.s, &dep.r, C->insn, rci < 0 ? NAN : ex,
                        &(xts_temit_t){ .metric = "extra", .extra_json = buf, .status = xs });
  }
  return 0;
}

static int run_special(xts_ctx_t *ctx, int sp, int di)
{
  static sc_meas_t m[2];
  for (int v = 0; v < 2; v++) {
    sc_unit_t u;
    sc_unit_init(&u);
    u.special = sp;
    u.special_dep = v == 0;
    u.special_dist = sdists[di];
    sc_measure(ctx, &u, 30 + sdists[di], &m[v]);
  }
  double pen = m[0].cyc - m[1].cyc, err = sqrt(m[0].err * m[0].err + m[1].err * m[1].err);
  int st = xts_status_worst(m[0].status, m[1].status);
  char buf[800];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  xj_kv_str(&j, "producer", sp == 1 ? "mtctr" : "mtlr");
  xj_kv_str(&j, "consumer", sp == 1 ? "bctr" : "blr");
  xj_kv_str(&j, "slot", sp == 1 ? "CTR" : "LR");
  xj_kv_str(&j, "control", "b to the same word");
  xj_kv_int(&j, "distance", sdists[di]);
  xj_kv_double(&j, "penalty_err", err);
  xj_kv_str(&j, "verdict", pen_verdict(pen, err, st, "stall", "none", "NEGATIVE"));
  if (sp == 1) xj_kv_str(&j, "note", "timed in the GPR loop frame: CTR belongs to the unit in both kernels");
  xj_finish(&j);
  xts_time_emit_diff(ctx, &m[0].s, &m[0].r, &m[1].s, &m[1].r, sp == 1 ? XI_bcctrx : XI_bclrx,
                     &(xts_temit_t){ .metric = "penalty", .extra_json = buf });
  return 0;
}

static int bp_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (sc_classes_init() <= 0) return -1;
  if (c->arg[0] == BK_SPECIAL) return run_special(ctx, (int)c->arg[1], (int)c->arg[2]);
  return run_pair(ctx, c);
}

/* ---- distance-sweep summaries (family finish) ----
 * One entry per dist.<P>><C>.<slot>.d<N> penalty record of the row: the n-th record of a (condition, case) is repeat n. */
#define BP_MAXE 4096
typedef struct { int cs, sw, di, rep, status; double pen, err; } bp_ent_t;
static struct {
  int row, n, dropped;
  xts_condtab_t ct;
  bp_ent_t e[BP_MAXE];
  char prefix[NSW][96];
} bscan;

static int dist_cb(const char *line, void *ud)
{
  xts_rline_t l;
  (void)ud;
  if (xts_rline_parse(line, &l) < 0 || xts_rline_row(&l) != bscan.row || strcmp(l.metric, "penalty") ||
      strncmp(l.case_id, "dist.", 5))
    return 0;
  char *dot = strrchr(l.case_id, '.');
  if (!dot || dot[1] != 'd') return 0;
  int dist = atoi(dot + 2), di = -1, sw = -1;
  for (int k = 0; k < ND; k++)
    if (dists[k] == dist) di = k;
  size_t pl = (size_t)(dot - l.case_id);
  for (int s = 0; s < NSW; s++)
    if (strlen(bscan.prefix[s]) == pl && !strncmp(bscan.prefix[s], l.case_id, pl)) sw = s;
  if (di < 0 || sw < 0) return 0;
  int cs = xts_condtab_add(&bscan.ct, &l);
  if (cs < 0 || bscan.n >= BP_MAXE) { bscan.dropped++; return 0; }
  int rep = 0;
  for (int i = 0; i < bscan.n; i++)
    if (bscan.e[i].cs == cs && bscan.e[i].sw == sw && bscan.e[i].di == di) rep++;
  bp_ent_t *e = &bscan.e[bscan.n++];
  e->cs = cs; e->sw = sw; e->di = di; e->rep = rep;
  e->status = l.status;
  e->pen = l.value;
  e->err = xts_rline_num(&l, "penalty_err");
  return 0;
}

static void sweep_summary(xts_ctx_t *ctx, int cs, int rep, int sw)
{
  const bp_ent_t *g[ND] = { 0 };
  int any = 0, have = 1, all = XTS_ST_PASS;
  for (int i = 0; i < bscan.n; i++) {
    const bp_ent_t *e = &bscan.e[i];
    if (e->cs == cs && e->rep == rep && e->sw == sw) { g[e->di] = e; any = 1; }
  }
  if (!any) return;
  int pi = sc_class_find(sweeps[sw][0]), ci = sc_class_find(sweeps[sw][1]);
  if (!sc_class_ok(pi) || !sc_class_ok(ci)) return;
  const sc_class_t *P = &sc_classes[pi], *C = &sc_classes[ci];
  int op = xts_op_index(&xts_isa[C->insn], sweeps[sw][2]);
  if (op < 0) return;
  for (int k = 0; k < ND; k++) {
    if (!g[k]) { have = 0; continue; }
    all = xts_status_worst(all, g[k]->status);
  }
  int hidden = -1;
  for (int k = ND - 1; have && all == XTS_ST_PASS && k >= 0; k--) {
    if (g[k]->pen <= 2 * g[k]->err + 0.1) hidden = dists[k];
    else break;
  }
  char buf[1400], cid[96];
  xts_json_t j;
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  common_detail(&j, P, C, op, dists[ND - 1], g[ND - 1] ? g[ND - 1]->pen : NAN, g[ND - 1] ? g[ND - 1]->err : NAN);
  xj_key(&j, "distances");
  xj_arr(&j);
  for (int k = 0; k < ND; k++) xj_int(&j, dists[k]);
  xj_end(&j);
  xj_key(&j, "penalties");
  xj_arr(&j);
  for (int k = 0; k < ND; k++) xj_double(&j, g[k] ? g[k]->pen : NAN);
  xj_end(&j);
  xj_kv_str(&j, "verdict", !have ? "UNDETERMINED" : all != XTS_ST_PASS ? "UNDETERMINED" : hidden < 0 ? "not hidden" : "hidden");
  xj_kv_str(&j, "meaning", "fillers (1/cycle) between producer and consumer needed for no stall");
  if (!have) xj_kv_str(&j, "why", "some distances have no record in this row/condition (not run, skipped or given up)");
  xj_finish(&j);
  int s2 = !have ? XTS_ST_SKIP : all;
  if (s2 == XTS_ST_PASS && hidden < 0) s2 = XTS_ST_INFO;
  snprintf(cid, sizeof cid, "%s.d%d", bscan.prefix[sw], dists[ND - 1]);
  xts_record_t r;
  xts_record_init(&r);
  r.case_id = cid;
  r.mnem = xts_isa[C->insn].mnem;
  r.metric = "latency";
  r.value = hidden >= 0 ? hidden : NAN;
  r.unit = "cycles";
  r.status = s2;
  r.detail = buf;
  r.detail_truncated = j.truncated;
  xts_emit_for_cond(ctx, bscan.ct.text[cs], &r);
}

static int bp_finish(xts_ctx_t *ctx)
{
  if (sc_classes_init() <= 0) return -1;
  memset(&bscan, 0, sizeof bscan);
  bscan.row = ctx->fin->row;
  for (int s = 0; s < NSW; s++)
    snprintf(bscan.prefix[s], sizeof bscan.prefix[s], "dist.%s>%s.%s", sweeps[s][0], sweeps[s][1], sweeps[s][2]);
  xts_results_foreach(xts_family_name(ctx->fam), dist_cb, NULL);
  if (bscan.dropped)
    xts_emit_block_info(ctx, "dist.summary", "info", bscan.dropped, "count", "{\"why\":\"scan table full: records dropped\"}");
  for (int cs = 0; cs < bscan.ct.n; cs++) {
    int maxrep = -1;
    for (int i = 0; i < bscan.n; i++)
      if (bscan.e[i].cs == cs && bscan.e[i].rep > maxrep) maxrep = bscan.e[i].rep;
    for (int rep = 0; rep <= maxrep; rep++)
      for (int s = 0; s < NSW; s++) sweep_summary(ctx, cs, rep, s);
  }
  return 0;
}

XTS_FAMILY(bypass_matrix, .kind = "sched", .enumerate = bp_enum, .run = bp_run, .finish = bp_finish, .block_cases = 32,
           .desc = "producer x consumer x operand slot: stall when adjacent and extra over the same-file reference");
