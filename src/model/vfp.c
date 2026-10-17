/* Vector floating-point model (owner: mfp): AltiVec vaddfp ... vctsxs and the VMX128 mirrors (not the vmaddfp128 /
 * vmaddcfp128 / vnmsubfp128 / vmsum*fp128 group, which belongs to vmx128.c).
 * AltiVec PEM rules: round to nearest even only, no exception flags (FPSCR untouched), VSCR[NJ]=1 flushes denormal
 * inputs and results to a zero of the same sign, NJ=0 is IEEE (Java) mode with gradual underflow. NaN results are the
 * first NaN operand quieted (OR 0x00400000) in the order vA, vB, vC; invalid operations give 0x7FC00000.
 * Single-precision arithmetic is SoftFloat f32 (fused f32_mulAdd for vmaddfp/vnmsubfp); no host FPU arithmetic.
 * Estimates (vrefp vrsqrtefp vexptefp vlogefp + 128) are in vfp_est.c. */
#include "fp_core.h"
#include "vfp.h"

vops_t xv_ops(const xts_insn_t *d, uint32_t w)
{
  vops_t o;
  o.vd = xf_op(d, w, "VD");
  if (o.vd < 0) o.vd = xf_op(d, w, "VD128");
  o.va = xf_op(d, w, "VA");
  if (o.va < 0) o.va = xf_op(d, w, "VA128");
  o.vb = xf_op(d, w, "VB");
  if (o.vb < 0) o.vb = xf_op(d, w, "VB128");
  o.vc = xf_op(d, w, "VC");
  o.uimm = xf_op(d, w, "UIMM");
  return o;
}

static float32_t F(uint32_t v) { float32_t f; f.v = v; return f; }

/* ---- binary arithmetic ---- */
enum { V_ADD, V_SUB, V_MUL, V_MADD, V_NMSUB };

static uint32_t lane_arith(int k, uint32_t a, uint32_t b, uint32_t c, int nj)
{
  a = xv_nj(a, nj);
  b = xv_nj(b, nj);
  c = xv_nj(c, nj);
  uint32_t r;
  if (k >= V_MADD) {
    /* vD = vA*vC + vB (vnmsubfp: -(vA*vC - vB), negated after rounding); NaN order vA, vB, vC */
    if (xs_isnan(a)) return a | XS_QBIT;
    if (xs_isnan(b)) return b | XS_QBIT;
    if (xs_isnan(c)) return c | XS_QBIT;
    r = f32_mulAdd(F(a), F(c), F(k == V_NMSUB ? b ^ XS_SIGN : b)).v;
    if (k == V_NMSUB && !xs_isnan(r)) r ^= XS_SIGN;
    return xv_nj(r, nj);
  }
  if (xs_isnan(a)) return a | XS_QBIT;
  if (xs_isnan(b)) return b | XS_QBIT;
  switch (k) {
  case V_ADD: r = f32_add(F(a), F(b)).v; break;
  case V_SUB: r = f32_sub(F(a), F(b)).v; break;
  default: r = f32_mul(F(a), F(b)).v; break;
  }
  return xv_nj(r, nj);
}

static xts_mres_t varith(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int k)
{
  vops_t o = xv_ops(d, w);
  int nj = xv_njmode(in);
  xv_sf_begin();
  xts_vr_t A = in->vr[o.va], B = in->vr[o.vb], C = o.vc >= 0 ? in->vr[o.vc] : in->vr[0], R;
  for (int i = 0; i < 4; i++) R.w[i] = lane_arith(k, A.w[i], B.w[i], C.w[i], nj);
  out->vr[o.vd] = R;
  return XTS_M_OK;
}

#define VARITH(name, fn, k)                                                                                     \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return varith(d, w, in, out, k);                                                                            \
  }                                                                                                             \
  XTS_MODEL(name, fn)
VARITH("vaddfp", m_vaddfp, V_ADD);
VARITH("vsubfp", m_vsubfp, V_SUB);
VARITH("vmaddfp", m_vmaddfp, V_MADD);
VARITH("vnmsubfp", m_vnmsubfp, V_NMSUB);
VARITH("vaddfp128", m_vaddfp128, V_ADD);
VARITH("vsubfp128", m_vsubfp128, V_SUB);
VARITH("vmulfp128", m_vmulfp128, V_MUL);   /* = fma(A, B, -0) on hardware; identical to a rounded product */

/* ---- max / min ---- */
static uint32_t lane_maxmin(uint32_t a, uint32_t b, int nj, int max)
{
  a = xv_nj(a, nj);
  b = xv_nj(b, nj);
  if (xs_isnan(a)) return a | XS_QBIT;
  if (xs_isnan(b)) return b | XS_QBIT;
  if (xs_iszero(a) && xs_iszero(b)) return max ? (a & b) : (a | b);   /* max(+0,-0) = +0, min = -0 */
  int lt = f32_lt_quiet(F(a), F(b));
  return max ? (lt ? b : a) : (lt ? a : b);
}

static xts_mres_t vmaxmin(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int max)
{
  vops_t o = xv_ops(d, w);
  int nj = xv_njmode(in);
  xts_vr_t R;
  for (int i = 0; i < 4; i++) R.w[i] = lane_maxmin(in->vr[o.va].w[i], in->vr[o.vb].w[i], nj, max);
  out->vr[o.vd] = R;
  return XTS_M_OK;
}
#define VMAXMIN(name, fn, max)                                                                                  \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return vmaxmin(d, w, in, out, max);                                                                         \
  }                                                                                                             \
  XTS_MODEL(name, fn)
VMAXMIN("vmaxfp", m_vmaxfp, 1);
VMAXMIN("vminfp", m_vminfp, 0);
VMAXMIN("vmaxfp128", m_vmaxfp128, 1);
VMAXMIN("vminfp128", m_vminfp128, 0);

/* ---- compares (Rc: CR6 = all true || 0 || all false || 0; vcmpbfp: 0 0 all-in-bounds 0) ---- */
enum { C_EQ, C_GE, C_GT, C_B };

static xts_mres_t vcmp(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int k)
{
  vops_t o = xv_ops(d, w);
  int nj = xv_njmode(in);
  int all = 1, none = 1;
  xts_vr_t R;
  for (int i = 0; i < 4; i++) {
    uint32_t a = xv_nj(in->vr[o.va].w[i], nj), b = xv_nj(in->vr[o.vb].w[i], nj), r;
    int nan = xs_isnan(a) || xs_isnan(b);
    if (k == C_B) {
      if (nan) r = 0xC0000000u;
      else {
        r = f32_le_quiet(F(a), F(b)) ? 0 : 0x80000000u;
        if (!f32_le_quiet(F(b ^ XS_SIGN), F(a))) r |= 0x40000000u;
      }
      if (r) all = 0;
    } else {
      int t = 0;
      if (!nan) {
        switch (k) {
        case C_EQ: t = f32_eq(F(a), F(b)); break;
        case C_GE: t = f32_le_quiet(F(b), F(a)); break;
        default: t = f32_lt_quiet(F(b), F(a)); break;
        }
      }
      r = t ? 0xFFFFFFFFu : 0;
      if (t) none = 0;
      else all = 0;
    }
    R.w[i] = r;
  }
  out->vr[o.vd] = R;
  if (xf_rc(d, w)) xm_set_cr_field(out, 6, k == C_B ? (all ? 2u : 0u) : ((all ? 8u : 0u) | (none ? 2u : 0u)));
  return XTS_M_OK;
}
#define VCMP(name, fn, k)                                                                                       \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return vcmp(d, w, in, out, k);                                                                              \
  }                                                                                                             \
  XTS_MODEL(name, fn)
VCMP("vcmpeqfp", m_vcmpeqfp, C_EQ);
VCMP("vcmpgefp", m_vcmpgefp, C_GE);
VCMP("vcmpgtfp", m_vcmpgtfp, C_GT);
VCMP("vcmpbfp", m_vcmpbfp, C_B);
VCMP("vcmpeqfp128", m_vcmpeqfp128, C_EQ);
VCMP("vcmpgefp128", m_vcmpgefp128, C_GE);
VCMP("vcmpgtfp128", m_vcmpgtfp128, C_GT);
VCMP("vcmpbfp128", m_vcmpbfp128, C_B);

/* ---- round to integral ---- */
static xts_mres_t vrfi(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, uint_fast8_t mode)
{
  vops_t o = xv_ops(d, w);
  int nj = xv_njmode(in);
  xts_vr_t R;
  for (int i = 0; i < 4; i++) {
    uint32_t b = xv_nj(in->vr[o.vb].w[i], nj);
    R.w[i] = xs_isnan(b) ? b | XS_QBIT : xv_nj(f32_roundToInt(F(b), mode, false).v, nj);
  }
  out->vr[o.vd] = R;
  return XTS_M_OK;
}
#define VRFI(name, fn, mode)                                                                                    \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return vrfi(d, w, in, out, mode);                                                                           \
  }                                                                                                             \
  XTS_MODEL(name, fn)
VRFI("vrfin", m_vrfin, softfloat_round_near_even);
VRFI("vrfiz", m_vrfiz, softfloat_round_minMag);
VRFI("vrfip", m_vrfip, softfloat_round_max);
VRFI("vrfim", m_vrfim, softfloat_round_min);
VRFI("vrfin128", m_vrfin128, softfloat_round_near_even);
VRFI("vrfiz128", m_vrfiz128, softfloat_round_minMag);
VRFI("vrfip128", m_vrfip128, softfloat_round_max);
VRFI("vrfim128", m_vrfim128, softfloat_round_min);

/* ---- int -> float: round to nearest even, then exact division by 2^UIMM (the int result is 0 or >= 1) ---- */
static xts_mres_t vcfx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int sgn)
{
  vops_t o = xv_ops(d, w);
  xv_sf_begin();
  xts_vr_t R;
  for (int i = 0; i < 4; i++) {
    uint32_t v = in->vr[o.vb].w[i];
    uint32_t r = sgn ? i32_to_f32((int32_t)v).v : ui32_to_f32(v).v;
    if (!xs_iszero(r)) r -= (uint32_t)o.uimm << 23;
    R.w[i] = r;
  }
  out->vr[o.vd] = R;
  return XTS_M_OK;
}

/* ---- float -> int: truncate toward zero after scaling by 2^UIMM, saturate (VSCR[SAT]); NaN -> 0 without SAT ---- */
static xts_mres_t vctx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int sgn)
{
  vops_t o = xv_ops(d, w);
  int nj = xv_njmode(in), sat = 0;
  xts_vr_t R;
  for (int i = 0; i < 4; i++) {
    uint32_t b = xv_nj(in->vr[o.vb].w[i], nj), r;
    int s = (int)(b >> 31), be = (int)((b >> 23) & 0xFF);
    if (xs_isnan(b)) { R.w[i] = 0; continue; }
    uint64_t mag = 0;
    int big = 0;
    if (be == 0xFF) big = 1;                       /* infinity */
    else if (be) {
      int e = be - 127 + o.uimm;                   /* value = 1.f * 2^e */
      uint64_t mnt = (b & 0x7FFFFFu) | 0x800000u;
      if (e >= 33) big = 1;
      else if (e >= 23) mag = mnt << (e - 23);
      else if (e >= 0) mag = mnt >> (23 - e);
    }                                              /* denormal (NJ=0): |x| * 2^31 < 1 -> 0 */
    if (sgn) {
      if (s) {
        if (big || mag > 0x80000000ull) { r = 0x80000000u; sat = 1; }
        else r = (uint32_t)(0 - mag);
      } else {
        if (big || mag > 0x7FFFFFFFull) { r = 0x7FFFFFFFu; sat = 1; }
        else r = (uint32_t)mag;
      }
    } else {
      if (s) {
        r = 0;
        if (big || mag) sat = 1;                   /* <= -1 saturates to 0; (-1, 0) truncates to 0 */
      } else if (big || mag > 0xFFFFFFFFull) { r = 0xFFFFFFFFu; sat = 1; }
      else r = (uint32_t)mag;
    }
    R.w[i] = r;
  }
  out->vr[o.vd] = R;
  if (sat) out->vscr.w[3] |= XTS_VSCR_SAT;
  return XTS_M_OK;
}
#define VCVT(name, fn, f, sgn)                                                                                  \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return f(d, w, in, out, sgn);                                                                               \
  }                                                                                                             \
  XTS_MODEL(name, fn)
VCVT("vcfux", m_vcfux, vcfx, 0);
VCVT("vcfsx", m_vcfsx, vcfx, 1);
VCVT("vctuxs", m_vctuxs, vctx, 0);
VCVT("vctsxs", m_vctsxs, vctx, 1);
VCVT("vcuxwfp128", m_vcuxwfp128, vcfx, 0);
VCVT("vcsxwfp128", m_vcsxwfp128, vcfx, 1);
VCVT("vcfpuxws128", m_vcfpuxws128, vctx, 0);
VCVT("vcfpsxws128", m_vcfpsxws128, vctx, 1);
