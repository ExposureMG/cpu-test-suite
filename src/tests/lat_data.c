/* lat.data - data-dependent cost per operand bin, as a slowdown ratio against the class's normal bin.
 *
 * Bins (source operands in asm order get v0, v1, v2):
 *   int   small (7,3)  wide (0x7FFFFFFFFFFFFFF1, 0xF0F0F0F3)  neg (-7,-3)  zero (0,0)                  normal = small
 *   mul   small  wide  w32 (0x7FFFFFF1, 0x7FFFFFF3)  w32_a (0x7FFFFFF1, 3)  w32_b (7, 0x7FFFFFF3)  neg  normal = small
 *   div   the OpenXenos DIV_DATASETS bins of divw/divwu/divd/divdu (small wide wide_wide neg_wide by_one quot0 w32)
 *   FPR   normal (1.5, 2.5)  denormal_in (both denormal)  denormal_out (normal inputs, denormal result; per family:
 *         add/sub cancellation, mul, div, fma, frsp, fres)  nan  inf  zero  [+ neg for sqrt/estimates]
 *   VR    normal  denormal  denormal_out  special (NaN, Inf)  [+ zero, neg for estimates], each at VSCR[NJ] = 0 and 1
 *         (only the axis value when the vscr_nj axis is applied to the block)
 * Shapes:
 *   indep  8 independent instructions (16 for tied VMX128 accumulators) with distinct destinations reading the SAME fixed
 *          source registers: per-instruction cost with that data (= latency for unpipelined dividers). metric issue_cost.
 *   chain  FP ops with an identity recipe keep the chained value in its bin (x*1, x/1, x+(-0), x*1+(-0), max(x,-inf),
 *          fsel, moves): dependent latency with that data. metric latency.
 * Every bin is measured together with the normal bin of the same shape (same NJ) and a "ratio" record (bin / normal) is
 * emitted per shape. FPSCR = 0 (no enabled exceptions), XER = 0. Soft baselines: JSON timing.fixed_operands.<bin>
 * (normal, wide, denormal, denormal_nj, special) for the indep shape of untied instructions. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "timing_ops.h"

enum { K_INT = 1, K_MUL, K_DIV, K_FPR, K_VR };

typedef struct ld_bin { const char *name; uint64_t v[3]; const char *base; } ld_bin_t;

#define NEG(x) ((uint64_t)(int64_t)(x))
static const ld_bin_t int_bins[] = {
  { "small", { 7, 3, 7 }, "fixed_operands.normal" },
  { "wide", { 0x7FFFFFFFFFFFFFF1ull, 0x00000000F0F0F0F3ull, 0x7FFFFFFFFFFFFFF1ull }, "fixed_operands.wide" },
  { "neg", { NEG(-7), NEG(-3), NEG(-7) }, NULL },
  { "zero", { 0, 0, 0 }, NULL },
};
static const ld_bin_t mul_bins[] = {
  { "small", { 7, 3, 7 }, "fixed_operands.normal" },
  { "wide", { 0x7FFFFFFFFFFFFFF1ull, 0x00000000F0F0F0F3ull, 0 }, "fixed_operands.wide" },
  { "w32", { 0x7FFFFFF1, 0x7FFFFFF3, 0 }, NULL },
  { "w32_a", { 0x7FFFFFF1, 3, 0 }, NULL },
  { "w32_b", { 7, 0x7FFFFFF3, 0 }, NULL },
  { "neg", { NEG(-7), NEG(-3), 0 }, NULL },
};
/* OpenXenos hwtest/host/plans.py DIV_DATASETS (dividend RA, divisor RB) */
static const ld_bin_t divw_bins[] = {
  { "small", { 7, 3 }, "fixed_operands.normal" }, { "wide", { 0x7FFFFFF1, 3 }, NULL },
  { "wide_wide", { 0x7FFFFFF1, 0x7FF1 }, NULL }, { "neg_wide", { 0xFFFFFFFF80000011ull, 7 }, NULL },
  { "by_one", { 0x7FFFFFF1, 1 }, NULL }, { "quot0", { 5, 0x7FFFFFF1 }, NULL },
};
static const ld_bin_t divwu_bins[] = {
  { "small", { 7, 3 }, "fixed_operands.normal" }, { "wide", { 0xFFFFFFF1, 3 }, NULL },
  { "wide_wide", { 0xFFFFFFF1, 0xFFF1 }, NULL }, { "by_one", { 0xFFFFFFF1, 1 }, NULL },
  { "quot0", { 5, 0xFFFFFFF1 }, NULL },
};
static const ld_bin_t divd_bins[] = {
  { "small", { 7, 3 }, "fixed_operands.normal" }, { "w32", { 0x7FFFFFF1, 3 }, NULL },
  { "wide", { 0x7FFFFFFFFFFFFFF1ull, 3 }, NULL },
};
static const ld_bin_t divdu_bins[] = {
  { "small", { 7, 3 }, "fixed_operands.normal" }, { "w32", { 0xFFFFFFF1, 3 }, NULL },
  { "wide", { 0xFFFFFFFFFFFFFFF1ull, 3 }, NULL },
};
/* FP bins: values computed per operand role (ld_fpval) */
enum { B_NORMAL, B_DEN_IN, B_DEN_OUT, B_NAN, B_INF, B_ZERO, B_NEG, B_SPECIAL, B_NFP };
static const char *const fp_bin_name[B_NFP] = { "normal", "denormal_in", "denormal_out", "nan", "inf", "zero", "neg",
                                                 "special" };
static const char *const vr_bin_name[B_NFP] = { "normal", "denormal", "denormal_out", "nan", "inf", "zero", "neg",
                                                 "special" };

static const char *const mul_mnems[] = { "mullwx", "mulhwx", "mulhwux", "mulldx", "mulhdx", "mulhdux", "mulli", NULL };

static int in_list(const char *const *l, const char *s)
{
  for (; *l; l++)
    if (!strcmp(*l, s)) return 1;
  return 0;
}

/* class of the instruction for lat.data (0 = not covered) and its fixed bin table */
static int ld_kind(int insn, const ld_bin_t **tab, int *n)
{
  tlat_shape_t s;
  if (tlat_shape(insn, &s, NULL) < 0) return 0;
  const char *m = xts_isa[insn].mnem;
  *tab = NULL;
  *n = 0;
  if (s.cls == TLAT_GPR) {
#define T(t) (*tab = t, *n = (int)(sizeof t / sizeof t[0]))
    if (!strcmp(m, "divwx")) { T(divw_bins); return K_DIV; }
    if (!strcmp(m, "divwux")) { T(divwu_bins); return K_DIV; }
    if (!strcmp(m, "divdx")) { T(divd_bins); return K_DIV; }
    if (!strcmp(m, "divdux")) { T(divdu_bins); return K_DIV; }
    if (in_list(mul_mnems, m)) { T(mul_bins); return K_MUL; }
    T(int_bins);
#undef T
    return K_INT;
  }
  const tlat_fpinfo_t *fi = tlat_fpinfo(insn);
  if (!fi || fi->fam == TLAT_FAM_I2F) return 0;       /* integer inputs */
  if (s.cls == TLAT_FPR) return K_FPR;
  if (s.cls == TLAT_VR) return K_VR;
  return 0;
}

/* ---- FP values ---- */
static uint64_t pow2bits(int vec, int e, int neg, int m15)
{
  uint64_t b = vec ? ((uint64_t)(e + 127) << 23) | (m15 ? 1u << 22 : 0) | (neg ? 0x80000000u : 0)
                   : ((uint64_t)(e + 1023) << 52) | (m15 ? 1ull << 51 : 0) | (neg ? 0x8000000000000000ull : 0);
  return b;
}
#define F(vec, d, f) ((vec) ? (uint64_t)(f) : (uint64_t)(d))

static int has_den_out(const tlat_fpinfo_t *fi, int insn)
{
  const char *m = xts_isa[insn].mnem;
  tlat_shape_t s;
  /* a tied accumulator feeds its own result back: the output is denormal only on the first round */
  if (tlat_shape(insn, &s, NULL) < 0 || s.tied) return 0;
  switch (fi->fam) {
  case TLAT_FAM_ADD: case TLAT_FAM_MUL: case TLAT_FAM_DIV: case TLAT_FAM_FMA: return 1;
  case TLAT_FAM_ROUND: return !strcmp(m, "frspx");
  case TLAT_FAM_EST: return !strcmp(m, "fresx") || !strncmp(m, "vrefp", 5) || !strncmp(m, "vexptefp", 8);
  default: return 0;
  }
}

static int bin_applies(int kind, const tlat_fpinfo_t *fi, int insn, int b)
{
  int est = fi->fam == TLAT_FAM_EST || fi->fam == TLAT_FAM_SQRT;
  switch (b) {
  case B_NORMAL: case B_DEN_IN: return 1;
  case B_DEN_OUT: return has_den_out(fi, insn);
  case B_NAN: case B_INF: return kind == K_FPR;
  case B_ZERO: return kind == K_FPR || est;
  case B_NEG: return est;
  case B_SPECIAL: return kind == K_VR;
  }
  return 0;
}

/* value of operand `opidx` (srcpos = its position among the sources, 0 for the accumulator) */
static uint64_t ld_fpval(const tlat_fpinfo_t *fi, const tlat_shape_t *s, int b, int opidx, int srcpos)
{
  int vec = s->cls == TLAT_VR, sgl = vec || fi->single;
  uint64_t qnan = F(vec, 0x7FF8000000000000ull, 0x7FC00000u), inf = F(vec, 0x7FF0000000000000ull, 0x7F800000u);
  switch (b) {
  case B_NORMAL:
    /* tied multiplicand accumulator (vmaddcfp128: VD = VA*VD + VB): VA = 1.0 so VD grows linearly instead of
     * overflowing to inf within a run */
    if ((fi->mul_ops & 1u) && opidx != s->dest && opidx != s->tied_alias && (fi->mul_ops & (1u << opidx)))
      return F(vec, 0x3FF0000000000000ull, 0x3F800000u);
    return srcpos == 1 ? F(vec, 0x4004000000000000ull, 0x40200000u) : F(vec, 0x3FF8000000000000ull, 0x3FC00000u);
  case B_DEN_IN: return srcpos == 1 ? F(vec, 0x0008000000000001ull, 0x00400001u) : F(vec, 0x000FFFFFFFFFFFFFull, 0x007FFFFFu);
  case B_NAN: return qnan;
  case B_INF: return inf;
  case B_ZERO: return 0;
  case B_NEG: return F(vec, 0xBFF8000000000000ull, 0xBFC00000u);
  case B_SPECIAL: return srcpos == 1 ? inf : qnan;
  case B_DEN_OUT: break;
  }
  const char *m = xts_isa[s->insn].mnem;
  switch (fi->fam) {
  case TLAT_FAM_ADD:     /* 2^-1021 - 1.5*2^-1022 = 2^-1023 (single: 2^-125 - 1.5*2^-126) */
    if (srcpos == 0) return pow2bits(vec, sgl ? -125 : -1021, 0, 0);
    return pow2bits(vec, sgl ? -126 : -1022, !fi->sub, 1);
  case TLAT_FAM_MUL:     /* 2^-600 * 2^-500 (single: 2^-70 * 2^-70) */
    return pow2bits(vec, sgl ? -70 : (srcpos == 0 ? -600 : -500), 0, 0);
  case TLAT_FAM_DIV:     /* 2^-1000 / 2^100 (single: 2^-100 / 2^40) */
    return srcpos == 0 ? pow2bits(vec, sgl ? -100 : -1000, 0, 0) : pow2bits(vec, sgl ? 40 : 100, 0, 0);
  case TLAT_FAM_FMA: {   /* 2^-511 * 2^-510 -/+ 1.5*2^-1022 = 2^-1023 (single: 2^-63 * 2^-62 -/+ 1.5*2^-126) */
    if (fi->mul_ops & (1u << opidx)) {
      int ord = 0;     /* ordinal among the multiplicands (operand-index order, the VS128 alias excluded) */
      for (int i = 0; i < opidx; i++)
        if ((fi->mul_ops & (1u << i)) && i != s->tied_alias) ord++;
      return pow2bits(vec, sgl ? (ord ? -62 : -63) : (ord ? -510 : -511), 0, 0);
    }
    return pow2bits(vec, sgl ? -126 : -1022, !fi->sub, 1);
  }
  case TLAT_FAM_ROUND: return pow2bits(0, -140, 0, 0);                         /* frsp: single denormal */
  case TLAT_FAM_EST:
    if (!strncmp(m, "vexptefp", 8)) return 0xC3020000u;                        /* 2^-130 */
    return pow2bits(vec, 127, 0, 0);                                            /* 1/2^127 */
  }
  return 0;
}

static tlat_val_t mkval(const tlat_shape_t *s, uint64_t bits)
{
  return s->cls == TLAT_VR ? tlat_fv((uint32_t)bits) : tlat_gv(bits);
}

/* ---- cases ---- */
static int ld_enum(xts_caselist_t *cl)
{
  for (int id = 0; id < xts_isa_count; id++) {
    const ld_bin_t *tab;
    int n, kind = ld_kind(id, &tab, &n);
    if (!kind) continue;
    if (kind == K_FPR || kind == K_VR) {
      const tlat_fpinfo_t *fi = tlat_fpinfo(id);
      for (int b = 0; b < B_NFP; b++) {
        if (!bin_applies(kind, fi, id, b)) continue;
        xts_case_t *c = xts_case_addf(cl, "%s:%s", xts_isa[id].mnem, kind == K_VR ? vr_bin_name[b] : fp_bin_name[b]);
        if (!c) return -1;
        c->insn = id;
        c->arg[0] = b;
        c->arg[1] = kind;
      }
    } else {
      tlat_shape_t s;
      tlat_shape(id, &s, NULL);
      for (int b = 0; b < n; b++) {
        if (s.nsrc < 2 && tab[b].v[0] == tab[0].v[0] && b) continue;    /* single source: same as the normal bin */
        xts_case_t *c = xts_case_addf(cl, "%s:%s", xts_isa[id].mnem, tab[b].name);
        if (!c) return -1;
        c->insn = id;
        c->arg[0] = b;
        c->arg[1] = kind;
      }
    }
  }
  return 0;
}

/* value of an operand for (kind, bin) */
static tlat_val_t opval(int kind, const ld_bin_t *tab, const tlat_shape_t *s, int b, int opidx, int srcpos)
{
  if (kind == K_FPR || kind == K_VR) return mkval(s, ld_fpval(tlat_fpinfo(s->insn), s, b, opidx, srcpos));
  return tlat_gv(tab[b].v[srcpos < 3 ? srcpos : 0]);
}

/* indep: P independent instructions, distinct destinations, shared fixed sources */
static int build_indep(int kind, const ld_bin_t *tab, const tlat_shape_t *s, int b, int nj, tlat_kern_t *k)
{
  int P = s->tied ? 16 : 8;
  tlat_kern_init(k, "indep", P);
  int dreg[16], sreg[TLAT_MAXSRC] = { 0 }, idx = 0;
  int vr128 = s->vr128, cls = s->cls;
  for (int c = 0; c < P; c++) {
    dreg[c] = tlat_pool(cls, vr128, idx++);
    if (dreg[c] < 0) return -1;
    tlat_kern_set(k, cls, dreg[c], opval(kind, tab, s, b, s->dest, 0));   /* tied accumulators start at v0 */
  }
  for (int jj = 0; jj < s->nsrc; jj++) {
    sreg[jj] = tlat_is_vrc(s, s->src[jj]) ? 7 : tlat_pool(cls, vr128, idx++);
    if (sreg[jj] < 0) return -1;
    tlat_kern_set(k, cls, sreg[jj], opval(kind, tab, s, b, s->src[jj], jj));
  }
  int64_t ops[XTS_MAX_OPS];
  for (int c = 0; c < P; c++) {
    if (tlat_assign(s, dreg[c], sreg, ops) < 0) return -1;
    tlat_kern_word(k, s->insn, 0, ops);
  }
  k->vscr = nj ? XTS_VSCR_NJ : 0;
  k->chain_reg[0] = dreg[0];
  return k->err;
}

/* chain with an identity recipe; returns 1 if built, 0 if the instruction has none, <0 on error */
static int build_chain(const tlat_shape_t *s, int b, int nj, tlat_kern_t *k)
{
  const tlat_fpinfo_t *fi = tlat_fpinfo(s->insn);
  if (!fi || b == B_DEN_OUT || b == B_NEG) return 0;
  /* scalar single-precision ops turn operands that are not valid singles (denormal) into a QNaN (results4): a chain
   * cannot stay in the denormal bin, only the indep shape measures it */
  if (s->cls == TLAT_FPR && fi->single && b == B_DEN_IN) return 0;
  int slot;
  switch (fi->fam) {
  case TLAT_FAM_ADD: case TLAT_FAM_MUL: case TLAT_FAM_DIV: case TLAT_FAM_MOVE: case TLAT_FAM_MINMAX: slot = 0; break;
  case TLAT_FAM_FMA: slot = s->tied ? TLAT_SLOT_ACC : 0; break;
  case TLAT_FAM_SEL: slot = 1; break;      /* fsel FD,FA,FC,FB: FA >= 0 selects FC */
  default: return 0;
  }
  if (tlat_build_chain(s, 0, slot, k) < 0) return -1;
  int vec = s->cls == TLAT_VR;
  uint64_t one = F(vec, 0x3FF0000000000000ull, 0x3F800000u), pz = 0, mz = F(vec, 0x8000000000000000ull, 0x80000000u);
  uint64_t pinf = F(vec, 0x7FF0000000000000ull, 0x7F800000u), minf = F(vec, 0xFFF0000000000000ull, 0xFF800000u);
  uint64_t cv = ld_fpval(fi, s, b, slot == TLAT_SLOT_ACC ? s->dest : s->src[slot], 0);
  tlat_kern_set(k, s->cls, k->chain_reg[0], mkval(s, cv));
  for (int jj = 0; jj < s->nsrc; jj++) {
    if (jj == slot) continue;
    int op = s->src[jj];
    uint64_t v = one;
    switch (fi->fam) {
    case TLAT_FAM_ADD: v = fi->sub ? pz : mz; break;
    case TLAT_FAM_MINMAX: v = fi->sub ? pinf : minf; break;      /* sub flag marks min */
    case TLAT_FAM_FMA:
      if (s->tied && !(fi->mul_ops & 1u)) v = (op == s->src[0]) ? one : (fi->sub ? pz : mz);   /* acc is the addend */
      else if (fi->mul_ops & (1u << op)) v = one;
      else v = fi->sub ? pz : mz;
      break;
    default: v = one; break;
    }
    tlat_kern_set(k, s->cls, k->op_reg[op], mkval(s, v));
  }
  k->vscr = nj ? XTS_VSCR_NJ : 0;
  return 1;
}

static void op_regs(tlat_kern_t *k, const tlat_shape_t *s)
{
  const xts_insn_t *d = &xts_isa[s->insn];
  for (int i = 0; i < XTS_MAX_OPS; i++) k->op_reg[i] = -1;
  for (int i = 0; i < d->nops && k->npat > 0; i++)
    if (i == s->dest || i == s->tied_alias) k->op_reg[i] = (int)k->pat[0].ops[i];
    else
      for (int jj = 0; jj < s->nsrc; jj++)
        if (s->src[jj] == i) k->op_reg[i] = (int)k->pat[0].ops[i];
}

/* for the host tests: the kernels a case measures (bin of the case, or its normal bin when `normal` is set) */
int tlat_ld_kernel(const xts_case_t *c, int chain, int normal, int nj, tlat_kern_t *k)
{
  tlat_shape_t s;
  const ld_bin_t *tab;
  int ntab, kind = (int)c->arg[1], b = (int)c->arg[0];
  if (tlat_shape(c->insn, &s, NULL) < 0 || ld_kind(c->insn, &tab, &ntab) != kind) return -1;
  if (normal) b = (kind == K_FPR || kind == K_VR) ? B_NORMAL : 0;
  if (!chain) {
    int e = build_indep(kind, tab, &s, b, nj, k);
    if (e == 0) op_regs(k, &s);
    return e < 0 ? -1 : 1;
  }
  if (kind != K_FPR && kind != K_VR) return 0;
  return build_chain(&s, b, nj, k);
}

static double est_cost(int kind, int insn, int b, int chain)
{
  const xts_insn_t *d = &xts_isa[insn];
  double lat = d->lat > 0 ? d->lat : 4, iss = d->issue > 0 ? d->issue : 1.2;
  int slow = (kind == K_FPR || kind == K_VR) && b != B_NORMAL;
  if (kind == K_DIV) return 50;
  if (slow) return 60;
  return chain ? lat : iss;
}

typedef struct meas { xts_tspec_t sp; xts_tresult_t r; tlat_kern_t k; double v; } meas_t;

/* v = cycles per instruction (NaN when not executed or failed) */
static void measure(meas_t *m, xts_ctx_t *ctx, double est_insn, const char *unit)
{
  tlat_spec(&m->sp, &m->k, est_insn * m->k.per_unit, unit);
  xts_time_measure(&m->sp, ctx, &m->r);          /* a failure is recorded by the emitter (r.failed) */
  m->v = xts_time_cycles(&m->r, m->k.per_unit);
}

static void common_detail(xts_json_t *j, int kind, const char *bin, int nj, const char *shape, const tlat_kern_t *k,
                          const tlat_shape_t *s)
{
  xj_kv_str(j, "bin", bin);
  xj_kv_str(j, "shape", shape);
  xj_kv_str(j, "class", kind == K_INT ? "int" : kind == K_MUL ? "mul" : kind == K_DIV ? "div" : kind == K_FPR ? "fpr" : "vr");
  if (kind == K_VR) xj_kv_int(j, "vscr_nj", nj);
  const tlat_fpinfo_t *fi = tlat_fpinfo(s->insn);
  if (fi) xj_kv_str(j, "family", tlat_fam_name(fi->fam));
  xj_kv_int(j, "per_unit", k->per_unit);
  xj_key(j, "values");
  xj_obj(j);
  const xts_insn_t *d = &xts_isa[s->insn];
  for (int i = 0; i < d->nops; i++) {
    int r = k->op_reg[i];
    if (r < 0 || i == s->tied_alias) continue;
    for (int q = 0; q < k->nset; q++) {
      if (k->set[q].reg != r || k->set[q].cls != s->cls) continue;
      if (s->cls == TLAT_VR) xj_kv_hex(j, d->ops[i].name, k->set[q].val.v.w[0], 8);
      else xj_kv_hex(j, d->ops[i].name, k->set[q].val.u, 16);
      break;
    }
  }
  xj_end(j);
}

/* detail members of the cost and ratio records of one (bin, NJ, shape) */
typedef struct ld_det {
  int kind, nj, forced, ratio;
  const char *bin, *shape;
  const tlat_kern_t *k;
  const tlat_shape_t *s;
  double v, vn;                       /* cycles with this bin / with the normal bin (NaN: not measured / same bin) */
} ld_det_t;

static void ld_detail(xts_json_t *j, void *arg)
{
  const ld_det_t *d = arg;
  common_detail(j, d->kind, d->bin, d->nj, d->shape, d->k, d->s);
  if (d->ratio) {
    xj_kv_str(j, "meaning", !strcmp(d->shape, "chain") ? "dependent latency with this bin / with the normal bin"
                                                        : "independent-issue cost with this bin / with the normal bin");
    xj_kv_double(j, "cycles", d->v);
    xj_kv_double(j, "normal_cycles", d->vn);
    return;
  }
  if (d->forced) xj_kv_bool(j, "vscr_nj_axis", 1);
  if (!isnan(d->vn)) xj_kv_double(j, "normal_cycles", d->vn);
}

static int ld_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static meas_t mb, mn;
  tlat_shape_t s;
  int b = (int)c->arg[0], kind = (int)c->arg[1];
  const ld_bin_t *tab;
  int ntab;
  if (tlat_shape(c->insn, &s, NULL) < 0 || ld_kind(c->insn, &tab, &ntab) != kind) {
    xts_emit_status(ctx, XTS_ST_FAIL, "{\"error\":\"case no longer matches the instruction table\"}");
    return 0;
  }
  int isfp = kind == K_FPR || kind == K_VR;
  int normal = isfp ? B_NORMAL : 0;
  const char *bin = isfp ? (kind == K_VR ? vr_bin_name[b] : fp_bin_name[b]) : tab[b].name;
  int njv[2] = { 0, 1 }, nnj = kind == K_VR ? 2 : 1, forced = 0, fnj = 0;
  if (kind == K_VR && tlat_axis_nj(ctx, &fnj)) { njv[0] = fnj; nnj = 1; forced = 1; }
  for (int q = 0; q < nnj; q++) {
    int nj = njv[q];
    char cid[64];
    if (kind == K_VR) snprintf(cid, sizeof cid, "%s:nj%d", c->id, nj);
    else snprintf(cid, sizeof cid, "%s", c->id);
    for (int shape = 0; shape < 2; shape++) {      /* 0 indep, 1 chain */
      int eb, en = 0;
      if (shape == 0) {
        eb = build_indep(kind, tab, &s, b, nj, &mb.k);
        if (!eb && b != normal) en = build_indep(kind, tab, &s, normal, nj, &mn.k);
        if (eb < 0 || en < 0) {
          xts_emit_status(ctx, XTS_ST_FAIL, "{\"error\":\"indep kernel build failed\"}");
          continue;
        }
        op_regs(&mb.k, &s);
      } else {
        if (!isfp) break;
        eb = build_chain(&s, b, nj, &mb.k);
        if (eb == 0) break;
        if (eb > 0 && b != normal) en = build_chain(&s, normal, nj, &mn.k);
        if (eb < 0 || en < 0) {
          xts_emit_status(ctx, XTS_ST_FAIL, "{\"error\":\"chain kernel build failed\"}");
          break;
        }
      }
      const char *shname = shape ? "chain" : "indep";
      measure(&mb, ctx, est_cost(kind, c->insn, b, shape), shname);
      if (b != normal) measure(&mn, ctx, est_cost(kind, c->insn, normal, shape), shname);
      double vn = b != normal ? mn.v : NAN;
      ld_det_t d = { .kind = kind, .nj = nj, .forced = forced, .bin = bin, .shape = shname, .k = &mb.k, .s = &s,
                     .v = mb.v, .vn = vn };
      /* cost record; soft baseline from the JSON fixed_operands for the indep shape of untied instructions */
      const char *bkey = NULL;
      if (shape == 0 && !s.tied) {
        if (!isfp) bkey = tab[b].base;
        else if (b == B_NORMAL && nj == 0) bkey = "fixed_operands.normal";
        else if (b == B_DEN_IN && kind == K_FPR) bkey = "fixed_operands.denormal";
        else if (b == B_DEN_IN && kind == K_VR) bkey = nj ? "fixed_operands.denormal_nj" : "fixed_operands.denormal";
        else if (b == B_SPECIAL && nj == 0) bkey = "fixed_operands.special";
      }
      xts_temit_t e = { .metric = shape ? "latency" : "issue_cost", .per_unit = mb.k.per_unit, .baseline_key = bkey,
                        .word = tlat_word0(&mb.k), .case_id = cid, .detail_fn = ld_detail, .detail_arg = &d };
      xts_time_emit_ex(ctx, &mb.sp, &mb.r, c->insn, &e);
      if (b == normal) continue;
      /* ratio record: this bin / the normal bin; fit = this bin, ref = the normal bin, status = the worse of both */
      d.ratio = 1;
      xts_temit_t er = { .metric = "ratio", .unit = "ratio", .per_unit = mb.k.per_unit, .word = e.word, .case_id = cid,
                         XTS_TE_VALUE((vn > 0 && !isnan(mb.v)) ? mb.v / vn : NAN),
                         .detail_fn = ld_detail, .detail_arg = &d };
      xts_time_emit_diff(ctx, &mb.sp, &mb.r, &mn.sp, &mn.r, c->insn, &er);
    }
  }
  return 0;
}

XTS_FAMILY(lat_data, .kind = "timing", .axes = "code_align,unroll,vscr_nj,sibling,other_cores,thread_prio", .enumerate = ld_enum,
           .run = ld_run, .block_cases = 16,
           .desc = "data-dependent cost per operand bin (int/mul/div/FP/vector FP x NJ), ratio vs the normal bin");
