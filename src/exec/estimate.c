/* Estimate-instruction verdict (xts/exec.h): the CPU result of fres/frsqrte/vrefp/vrsqrtefp/vexptefp/vlogefp (+128 forms)
 * against the architected accuracy bound when the reference model cannot give the exact (table) answer. Lanes the model
 * knows exactly (special operands/results) are found by probing the model with the other lanes set to +inf. */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <model/model_priv.h>

int xts_estimate_kind(int insn, int *vector)
{
  static const struct { const char *m; int kind, vec; } t[] = {
    { "fresx", 0, 0 }, { "frsqrtex", 1, 0 },
    { "vrefp", 0, 1 }, { "vrsqrtefp", 1, 1 }, { "vexptefp", 2, 1 }, { "vlogefp", 3, 1 },
    { "vrefp128", 0, 1 }, { "vrsqrtefp128", 1, 1 }, { "vexptefp128", 2, 1 }, { "vlogefp128", 3, 1 },
  };
  const xts_insn_t *d = xts_insn(insn);
  for (unsigned i = 0; d && i < sizeof t / sizeof t[0]; i++)
    if (!strcmp(d->mnem, t[i].m)) {
      if (vector) *vector = t[i].vec;
      return t[i].kind;
    }
  return -1;
}

/* single -> double bits, exact (denormals normalised) */
static uint64_t w2d(uint32_t w)
{
  uint64_t s = (uint64_t)(w >> 31) << 63;
  int e = (int)((w >> 23) & 0xFF);
  uint64_t f = w & 0x7FFFFFu;
  if (e == 0xFF) return s | 0x7FF0000000000000ull | (f << 29);
  if (e == 0) {
    if (!f) return s;
    int sh = 0;
    while (!(f & 0x800000u)) { f <<= 1; sh++; }
    f &= 0x7FFFFFu;
    return s | ((uint64_t)(1023 - 126 - sh) << 52) | (f << 29);
  }
  return s | ((uint64_t)(e - 127 + 1023) << 52) | (f << 29);
}

/* fres with FPSCR[OE] set and a reciprocal beyond the single range: Xenon does not deliver +-inf / max but the
 * exponent-adjusted result of the enabled-overflow rule, estimate * 2^-192 (console run, Jasper, FPSCR 0x40 and 0xF8,
 * MSR[FE0/FE1]=0 so no interrupt: |x| from 2^-1074 to 2^-238, both signs, all four results are the 8-bit table value
 * times 2^-192 with the sign of x). cpu = est(x) * 2^-192 = est(x * 2^192) holds the architected bound on the pair
 * (x * 2^192, cpu); x * 2^192 is always a normal double here (|x| < 2^-129). Returns 0 when cpu cannot be such a result
 * (zero, denormal, inf, NaN), else 1 and the pair's x in *xs. */
static int fres_oe_pair(uint64_t x, uint64_t cpu, uint64_t *xs)
{
  const int ce = (int)((cpu >> 52) & 0x7FF);
  if (ce == 0 || ce == 0x7FF) return 0;
  uint64_t f = x & 0xFFFFFFFFFFFFFull;
  int xe = (int)((x >> 52) & 0x7FF);
  if (!xe) {                                     /* denormal: normalise */
    int sh = __builtin_clzll(f) - 11;
    f = (f << sh) & 0xFFFFFFFFFFFFFull;
    xe = 1 - sh;
  }
  *xs = (x & (1ull << 63)) | ((uint64_t)(xe + 192) << 52) | f;
  return 1;
}

/* unbiased exponent of the leading bit of a finite nonzero double (denormals included) */
static int d_lead_exp(uint64_t x)
{
  int e = (int)((x >> 52) & 0x7FF);
  if (e) return e - 1023;
  uint64_t f = x & 0xFFFFFFFFFFFFFull;
  return (63 - __builtin_clzll(f)) - 1074;
}

static int op_reg(const xts_insn_t *d, uint32_t word, const char *a, const char *b)
{
  int i = xts_op_index(d, a);
  if (i < 0) i = xts_op_index(d, b);
  return i < 0 ? -1 : (int)xts_extract(d->index, word, i);
}

int xts_estimate_verdict(int insn, uint32_t word, const xts_state_t *in, const xts_state_t *out, xts_json_t *j)
{
  int vector = 0, kind = xts_estimate_kind(insn, &vector);
  const xts_insn_t *d = xts_insn(insn);
  if (kind < 0 || !d) {
    if (j) xj_kv_str(j, "estimate_error", "not an estimate instruction");
    return XTS_ST_FAIL;
  }
  int rt = vector ? op_reg(d, word, "VD", "VD128") : op_reg(d, word, "FD", "FD");
  int rb = vector ? op_reg(d, word, "VB", "VB128") : op_reg(d, word, "FB", "FB");
  if (rt < 0 || rb < 0) {
    if (j) xj_kv_str(j, "estimate_error", "operands not found");
    return XTS_ST_FAIL;
  }
  /* everything except the target (and FPSCR / CR1 for the scalar forms) must be unchanged */
  static xts_state_t exp;
  exp = *in;
  xm_state_loaded(&exp);
  if (vector) {
    exp.vr[rt & 127] = out->vr[rt & 127];
  } else {
    exp.fpr[rt & 31] = out->fpr[rt & 31];
    exp.fpscr = out->fpscr;
    if (xts_flags_of(insn, word) & XTS_RC) xm_set_cr_field(&exp, 1, xm_cr_field(out, 1));
  }
  xts_state_diff_t diffs[8];
  int nd = xts_state_compare(&exp, out, diffs, 8);
  int bad = nd > 0, checked = 0;
  if (j && nd) {
    xj_key(j, "unexpected_changes");
    xj_arr(j);
    for (int i = 0; i < nd && i < 8; i++) xj_str(j, diffs[i].what);
    xj_end(j);
  }
  if (j) {
    xj_kv_int(j, "estimate_kind", kind);
    xj_kv_int(j, "bound_bits", kind > 1 ? 0 : vector ? 12 : (kind == 0 ? 8 : 5));
    xj_key(j, "lanes");
    xj_arr(j);
  }
  int nl = vector ? 4 : 1;
  for (int i = 0; i < nl; i++) {
    uint64_t x, cpu, exact = 0;
    int verdict = 0, ok = -1, adj = 0;        /* verdict 0 unknown, 1 bound, 2 exact; adj: exponent adjust applied */
    if (vector) {
      x = in->vr[rb & 127].w[i];
      cpu = out->vr[rt & 127].w[i];
      /* the model knows this lane exactly if it answers with the other lanes at +inf */
      static xts_state_t p, po;
      p = *in;
      for (int k = 0; k < 4; k++)
        if (k != i) p.vr[rb & 127].w[k] = 0x7F800000u;
      xts_mem_t m;
      xts_mem_init(&m);
      if (xts_model_exec(d, word, &p, &m, &po) == XTS_M_OK) {
        verdict = 2;
        exact = po.vr[rt & 127].w[i];
        ok = cpu == exact;
      } else {
        int r = xf_estimate_ok(kind, 1, w2d((uint32_t)x), w2d((uint32_t)cpu));
        if (r >= 0) { verdict = 1; ok = r; }
      }
    } else {
      x = in->fpr[rb & 31];
      cpu = out->fpr[rt & 31];
      int r = xf_estimate_ok(kind, 0, x, cpu);
      const int finite = ((x >> 52) & 0x7FF) != 0x7FF && (x << 1) != 0;
      if (kind == 0 && finite && (in->fpscr & XTS_FPSCR_OE)) {
        /* enabled overflow: for |x| < 2^-129 the reciprocal is beyond the single range for certain and the result is the
         * estimate scaled by 2^-192; |x| in [2^-129, 2^-127) may or may not overflow, so either form is accepted. */
        int e = d_lead_exp(x);
        uint64_t xs;
        if (e <= -130) {
          r = fres_oe_pair(x, cpu, &xs) ? xf_estimate_ok(kind, 0, xs, cpu) : 0;
          if (r < 0) r = 0;
          adj = -192;
        } else if (e < -127 && r != 1 && fres_oe_pair(x, cpu, &xs)) {
          if (xf_estimate_ok(kind, 0, xs, cpu) == 1) { r = 1; adj = -192; }
        }
      }
      if (r >= 0) { verdict = 1; ok = r; }
    }
    if (verdict) checked++;
    if (ok == 0) bad = 1;
    if (j) {
      xj_obj(j);
      xj_kv_hex(j, "x", x, vector ? 8 : 16);
      xj_kv_hex(j, "cpu", cpu, vector ? 8 : 16);
      xj_kv_str(j, "verdict", verdict == 2 ? "exact" : verdict == 1 ? "bound" : "unknown");
      if (verdict == 2) xj_kv_hex(j, "exact", exact, 8);
      if (adj) xj_kv_int(j, "exp_adjust", adj);
      xj_key(j, "ok");
      if (ok < 0) xj_null(j);
      else xj_bool(j, ok);
      xj_end(j);
    }
  }
  if (j) xj_end(j);
  return bad ? XTS_ST_FAIL : checked ? XTS_ST_PASS : XTS_ST_INFO;
}
