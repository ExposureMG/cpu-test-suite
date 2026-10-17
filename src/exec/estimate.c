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
    int verdict = 0, ok = -1;                 /* verdict 0 unknown, 1 bound, 2 exact */
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
      xj_key(j, "ok");
      if (ok < 0) xj_null(j);
      else xj_bool(j, ok);
      xj_end(j);
    }
  }
  if (j) xj_end(j);
  return bad ? XTS_ST_FAIL : checked ? XTS_ST_PASS : XTS_ST_INFO;
}
