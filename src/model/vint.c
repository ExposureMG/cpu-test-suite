/* Vector integer model, part 1: element arithmetic, compares, shifts/rotates, logicals, splats, VSCR moves.
 * Covers the AltiVec VX/VC forms and their VMX128 counterparts (vand128, vrlw128, vcmpequw128, vspltw128, ...).
 * Semantics follow the AltiVec PEM / PowerISA 2.02 book 1 ch.6 with big-endian element numbering; every instruction
 * here is checked against the results4 hardware replay (tools/check_model.py). Saturating ops set VSCR[SAT]; compare
 * instructions with Rc=1 set CR field 6 (LT = all elements true, EQ = all false). Generated registration lines at the
 * end of each group: one XTS_MODEL per line (the macro keys on __LINE__). */
#include "vint_priv.h"

enum { OP_ADD, OP_ADDU_S, OP_ADDS_S, OP_SUB, OP_SUBU_S, OP_SUBS_S, OP_MAXU, OP_MAXS, OP_MINU, OP_MINS, OP_AVGU, OP_AVGS,
       OP_EQ, OP_GTU, OP_GTS, OP_RL, OP_SL, OP_SR, OP_SRA, OP_ADDC, OP_SUBC,
       OP_AND, OP_ANDC, OP_OR, OP_XOR, OP_NOR };

static int64_t clamp_s(int64_t v, int bits, int *sat)
{
  int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -hi - 1;
  if (v > hi) { *sat = 1; return hi; }
  if (v < lo) { *sat = 1; return lo; }
  return v;
}

/* one lane operation; returns the result (already truncated by vi_set), *sat |= saturation */
static uint32_t lane(int op, int esz, uint32_t ua, uint32_t ub, int *sat)
{
  int bits = 8 * esz;
  uint64_t mask = esz == 4 ? 0xFFFFFFFFull : (1ull << bits) - 1;
  int64_t sa = vi_sext(ua, esz), sb = vi_sext(ub, esz);
  uint32_t sh = ub & (uint32_t)(bits - 1);
  uint64_t t;
  switch (op) {
  case OP_ADD: return ua + ub;
  case OP_ADDU_S: t = (uint64_t)ua + ub; if (t > mask) { *sat = 1; return (uint32_t)mask; } return (uint32_t)t;
  case OP_ADDS_S: return (uint32_t)clamp_s(sa + sb, bits, sat);
  case OP_SUB: return ua - ub;
  case OP_SUBU_S: if (ua < ub) { *sat = 1; return 0; } return ua - ub;
  case OP_SUBS_S: return (uint32_t)clamp_s(sa - sb, bits, sat);
  case OP_MAXU: return ua > ub ? ua : ub;
  case OP_MAXS: return sa > sb ? ua : ub;
  case OP_MINU: return ua < ub ? ua : ub;
  case OP_MINS: return sa < sb ? ua : ub;
  case OP_AVGU: return (uint32_t)(((uint64_t)ua + ub + 1) >> 1);
  case OP_AVGS: return (uint32_t)((sa + sb + 1) >> 1);
  case OP_EQ: return ua == ub ? (uint32_t)mask : 0;
  case OP_GTU: return ua > ub ? (uint32_t)mask : 0;
  case OP_GTS: return sa > sb ? (uint32_t)mask : 0;
  case OP_RL: return sh ? (uint32_t)(((uint64_t)ua << sh | (uint64_t)ua >> (bits - sh)) & mask) : ua;
  case OP_SL: return (uint32_t)(((uint64_t)ua << sh) & mask);
  case OP_SR: return ua >> sh;
  case OP_SRA: return (uint32_t)(sa >> sh);
  case OP_ADDC: return (uint32_t)(((uint64_t)ua + ub) >> 32);           /* carry out of the unsigned word add */
  case OP_SUBC: return ua >= ub;                                          /* carry out of ua + ~ub + 1 */
  }
  return 0;
}

/* elementwise binary operation vD = vA op vB; x128 selects the VMX128 register fields; cmp: compare with Rc. */
static xts_mres_t vbin(int op, int esz, int x128, const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  const xts_vr_t *a = &in->vr[vi_va(w, x128)], *b = &in->vr[vi_vb(w, x128)];
  xts_vr_t r = { { 0, 0, 0, 0 } };
  int sat = 0;
  for (int i = 0; i < 16 / esz; i++) vi_set(&r, esz, i, lane(op, esz, vi_get(a, esz, i), vi_get(b, esz, i), &sat));
  out->vr[vi_vd(w, x128)] = r;
  if (sat) vi_set_sat(out);
  if ((op == OP_EQ || op == OP_GTU || op == OP_GTS) && (xts_flags_of(d->index, w) & XTS_RC)) {
    int all1 = 1, all0 = 1;
    for (int i = 0; i < 4; i++) { if (r.w[i] != 0xFFFFFFFFu) all1 = 0; if (r.w[i]) all0 = 0; }
    xm_set_cr_field(out, 6, (all1 ? 8u : 0u) | (all0 ? 2u : 0u));
  }
  return XTS_M_OK;
}

/* bitwise logicals */
static xts_mres_t vlogic(int op, int x128, const xts_state_t *in, uint32_t w, xts_state_t *out)
{
  const xts_vr_t *a = &in->vr[vi_va(w, x128)], *b = &in->vr[vi_vb(w, x128)];
  xts_vr_t r;
  for (int i = 0; i < 4; i++) {
    uint32_t x = a->w[i], y = b->w[i];
    r.w[i] = op == OP_AND ? x & y : op == OP_ANDC ? x & ~y : op == OP_OR ? x | y : op == OP_XOR ? x ^ y : ~(x | y);
  }
  out->vr[vi_vd(w, x128)] = r;
  return XTS_M_OK;
}

/* splat element UIMM (masked to the element count) of vB */
static xts_mres_t vsplat(int esz, int x128, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  const xts_vr_t *b = &in->vr[vi_vb(w, x128)];
  uint32_t x = vi_get(b, esz, (int)xts_field_get(XTS_F_UIMM5, w) & (16 / esz - 1));
  xts_vr_t r = { { 0, 0, 0, 0 } };
  for (int i = 0; i < 16 / esz; i++) vi_set(&r, esz, i, x);
  out->vr[vi_vd(w, x128)] = r;
  return XTS_M_OK;
}

/* splat sign-extended 5-bit immediate */
static xts_mres_t vsplati(int esz, int x128, uint32_t w, xts_state_t *out)
{
  uint32_t x = (uint32_t)xts_field_get(XTS_F_SIMM5, w);
  xts_vr_t r = { { 0, 0, 0, 0 } };
  for (int i = 0; i < 16 / esz; i++) vi_set(&r, esz, i, x);
  out->vr[vi_vd(w, x128)] = r;
  return XTS_M_OK;
}

static xts_mres_t m_mfvscr(VI_PARAMS)
{
  VI_UNUSED;
  out->vr[XM_RD(w)] = xts_vr4(0, 0, 0, in->vscr.w[3] & (XTS_VSCR_NJ | XTS_VSCR_SAT));
  return XTS_M_OK;
}
XTS_MODEL("mfvscr", m_mfvscr);

static xts_mres_t m_mtvscr(VI_PARAMS)
{
  VI_UNUSED;
  out->vscr = xts_vr4(0, 0, 0, in->vr[XM_RB(w)].w[3] & (XTS_VSCR_NJ | XTS_VSCR_SAT));
  return XTS_M_OK;
}
XTS_MODEL("mtvscr", m_mtvscr);

static xts_mres_t m_vaddubm(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADD, 1, 0, d, w, in, out); }
XTS_MODEL("vaddubm", m_vaddubm);

static xts_mres_t m_vsububm(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUB, 1, 0, d, w, in, out); }
XTS_MODEL("vsububm", m_vsububm);

static xts_mres_t m_vaddubs(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADDU_S, 1, 0, d, w, in, out); }
XTS_MODEL("vaddubs", m_vaddubs);

static xts_mres_t m_vsububs(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUBU_S, 1, 0, d, w, in, out); }
XTS_MODEL("vsububs", m_vsububs);

static xts_mres_t m_vaddsbs(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADDS_S, 1, 0, d, w, in, out); }
XTS_MODEL("vaddsbs", m_vaddsbs);

static xts_mres_t m_vsubsbs(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUBS_S, 1, 0, d, w, in, out); }
XTS_MODEL("vsubsbs", m_vsubsbs);

static xts_mres_t m_vmaxub(VI_PARAMS) { VI_UNUSED; return vbin(OP_MAXU, 1, 0, d, w, in, out); }
XTS_MODEL("vmaxub", m_vmaxub);

static xts_mres_t m_vmaxsb(VI_PARAMS) { VI_UNUSED; return vbin(OP_MAXS, 1, 0, d, w, in, out); }
XTS_MODEL("vmaxsb", m_vmaxsb);

static xts_mres_t m_vminub(VI_PARAMS) { VI_UNUSED; return vbin(OP_MINU, 1, 0, d, w, in, out); }
XTS_MODEL("vminub", m_vminub);

static xts_mres_t m_vminsb(VI_PARAMS) { VI_UNUSED; return vbin(OP_MINS, 1, 0, d, w, in, out); }
XTS_MODEL("vminsb", m_vminsb);

static xts_mres_t m_vavgub(VI_PARAMS) { VI_UNUSED; return vbin(OP_AVGU, 1, 0, d, w, in, out); }
XTS_MODEL("vavgub", m_vavgub);

static xts_mres_t m_vavgsb(VI_PARAMS) { VI_UNUSED; return vbin(OP_AVGS, 1, 0, d, w, in, out); }
XTS_MODEL("vavgsb", m_vavgsb);

static xts_mres_t m_vcmpequb(VI_PARAMS) { VI_UNUSED; return vbin(OP_EQ, 1, 0, d, w, in, out); }
XTS_MODEL("vcmpequb", m_vcmpequb);

static xts_mres_t m_vcmpgtub(VI_PARAMS) { VI_UNUSED; return vbin(OP_GTU, 1, 0, d, w, in, out); }
XTS_MODEL("vcmpgtub", m_vcmpgtub);

static xts_mres_t m_vcmpgtsb(VI_PARAMS) { VI_UNUSED; return vbin(OP_GTS, 1, 0, d, w, in, out); }
XTS_MODEL("vcmpgtsb", m_vcmpgtsb);

static xts_mres_t m_vrlb(VI_PARAMS) { VI_UNUSED; return vbin(OP_RL, 1, 0, d, w, in, out); }
XTS_MODEL("vrlb", m_vrlb);

static xts_mres_t m_vslb(VI_PARAMS) { VI_UNUSED; return vbin(OP_SL, 1, 0, d, w, in, out); }
XTS_MODEL("vslb", m_vslb);

static xts_mres_t m_vsrb(VI_PARAMS) { VI_UNUSED; return vbin(OP_SR, 1, 0, d, w, in, out); }
XTS_MODEL("vsrb", m_vsrb);

static xts_mres_t m_vsrab(VI_PARAMS) { VI_UNUSED; return vbin(OP_SRA, 1, 0, d, w, in, out); }
XTS_MODEL("vsrab", m_vsrab);

static xts_mres_t m_vadduhm(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADD, 2, 0, d, w, in, out); }
XTS_MODEL("vadduhm", m_vadduhm);

static xts_mres_t m_vsubuhm(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUB, 2, 0, d, w, in, out); }
XTS_MODEL("vsubuhm", m_vsubuhm);

static xts_mres_t m_vadduhs(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADDU_S, 2, 0, d, w, in, out); }
XTS_MODEL("vadduhs", m_vadduhs);

static xts_mres_t m_vsubuhs(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUBU_S, 2, 0, d, w, in, out); }
XTS_MODEL("vsubuhs", m_vsubuhs);

static xts_mres_t m_vaddshs(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADDS_S, 2, 0, d, w, in, out); }
XTS_MODEL("vaddshs", m_vaddshs);

static xts_mres_t m_vsubshs(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUBS_S, 2, 0, d, w, in, out); }
XTS_MODEL("vsubshs", m_vsubshs);

static xts_mres_t m_vmaxuh(VI_PARAMS) { VI_UNUSED; return vbin(OP_MAXU, 2, 0, d, w, in, out); }
XTS_MODEL("vmaxuh", m_vmaxuh);

static xts_mres_t m_vmaxsh(VI_PARAMS) { VI_UNUSED; return vbin(OP_MAXS, 2, 0, d, w, in, out); }
XTS_MODEL("vmaxsh", m_vmaxsh);

static xts_mres_t m_vminuh(VI_PARAMS) { VI_UNUSED; return vbin(OP_MINU, 2, 0, d, w, in, out); }
XTS_MODEL("vminuh", m_vminuh);

static xts_mres_t m_vminsh(VI_PARAMS) { VI_UNUSED; return vbin(OP_MINS, 2, 0, d, w, in, out); }
XTS_MODEL("vminsh", m_vminsh);

static xts_mres_t m_vavguh(VI_PARAMS) { VI_UNUSED; return vbin(OP_AVGU, 2, 0, d, w, in, out); }
XTS_MODEL("vavguh", m_vavguh);

static xts_mres_t m_vavgsh(VI_PARAMS) { VI_UNUSED; return vbin(OP_AVGS, 2, 0, d, w, in, out); }
XTS_MODEL("vavgsh", m_vavgsh);

static xts_mres_t m_vcmpequh(VI_PARAMS) { VI_UNUSED; return vbin(OP_EQ, 2, 0, d, w, in, out); }
XTS_MODEL("vcmpequh", m_vcmpequh);

static xts_mres_t m_vcmpgtuh(VI_PARAMS) { VI_UNUSED; return vbin(OP_GTU, 2, 0, d, w, in, out); }
XTS_MODEL("vcmpgtuh", m_vcmpgtuh);

static xts_mres_t m_vcmpgtsh(VI_PARAMS) { VI_UNUSED; return vbin(OP_GTS, 2, 0, d, w, in, out); }
XTS_MODEL("vcmpgtsh", m_vcmpgtsh);

static xts_mres_t m_vrlh(VI_PARAMS) { VI_UNUSED; return vbin(OP_RL, 2, 0, d, w, in, out); }
XTS_MODEL("vrlh", m_vrlh);

static xts_mres_t m_vslh(VI_PARAMS) { VI_UNUSED; return vbin(OP_SL, 2, 0, d, w, in, out); }
XTS_MODEL("vslh", m_vslh);

static xts_mres_t m_vsrh(VI_PARAMS) { VI_UNUSED; return vbin(OP_SR, 2, 0, d, w, in, out); }
XTS_MODEL("vsrh", m_vsrh);

static xts_mres_t m_vsrah(VI_PARAMS) { VI_UNUSED; return vbin(OP_SRA, 2, 0, d, w, in, out); }
XTS_MODEL("vsrah", m_vsrah);

static xts_mres_t m_vadduwm(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADD, 4, 0, d, w, in, out); }
XTS_MODEL("vadduwm", m_vadduwm);

static xts_mres_t m_vsubuwm(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUB, 4, 0, d, w, in, out); }
XTS_MODEL("vsubuwm", m_vsubuwm);

static xts_mres_t m_vadduws(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADDU_S, 4, 0, d, w, in, out); }
XTS_MODEL("vadduws", m_vadduws);

static xts_mres_t m_vsubuws(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUBU_S, 4, 0, d, w, in, out); }
XTS_MODEL("vsubuws", m_vsubuws);

static xts_mres_t m_vaddsws(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADDS_S, 4, 0, d, w, in, out); }
XTS_MODEL("vaddsws", m_vaddsws);

static xts_mres_t m_vsubsws(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUBS_S, 4, 0, d, w, in, out); }
XTS_MODEL("vsubsws", m_vsubsws);

static xts_mres_t m_vmaxuw(VI_PARAMS) { VI_UNUSED; return vbin(OP_MAXU, 4, 0, d, w, in, out); }
XTS_MODEL("vmaxuw", m_vmaxuw);

static xts_mres_t m_vmaxsw(VI_PARAMS) { VI_UNUSED; return vbin(OP_MAXS, 4, 0, d, w, in, out); }
XTS_MODEL("vmaxsw", m_vmaxsw);

static xts_mres_t m_vminuw(VI_PARAMS) { VI_UNUSED; return vbin(OP_MINU, 4, 0, d, w, in, out); }
XTS_MODEL("vminuw", m_vminuw);

static xts_mres_t m_vminsw(VI_PARAMS) { VI_UNUSED; return vbin(OP_MINS, 4, 0, d, w, in, out); }
XTS_MODEL("vminsw", m_vminsw);

static xts_mres_t m_vavguw(VI_PARAMS) { VI_UNUSED; return vbin(OP_AVGU, 4, 0, d, w, in, out); }
XTS_MODEL("vavguw", m_vavguw);

static xts_mres_t m_vavgsw(VI_PARAMS) { VI_UNUSED; return vbin(OP_AVGS, 4, 0, d, w, in, out); }
XTS_MODEL("vavgsw", m_vavgsw);

static xts_mres_t m_vcmpequw(VI_PARAMS) { VI_UNUSED; return vbin(OP_EQ, 4, 0, d, w, in, out); }
XTS_MODEL("vcmpequw", m_vcmpequw);

static xts_mres_t m_vcmpgtuw(VI_PARAMS) { VI_UNUSED; return vbin(OP_GTU, 4, 0, d, w, in, out); }
XTS_MODEL("vcmpgtuw", m_vcmpgtuw);

static xts_mres_t m_vcmpgtsw(VI_PARAMS) { VI_UNUSED; return vbin(OP_GTS, 4, 0, d, w, in, out); }
XTS_MODEL("vcmpgtsw", m_vcmpgtsw);

static xts_mres_t m_vrlw(VI_PARAMS) { VI_UNUSED; return vbin(OP_RL, 4, 0, d, w, in, out); }
XTS_MODEL("vrlw", m_vrlw);

static xts_mres_t m_vslw(VI_PARAMS) { VI_UNUSED; return vbin(OP_SL, 4, 0, d, w, in, out); }
XTS_MODEL("vslw", m_vslw);

static xts_mres_t m_vsrw(VI_PARAMS) { VI_UNUSED; return vbin(OP_SR, 4, 0, d, w, in, out); }
XTS_MODEL("vsrw", m_vsrw);

static xts_mres_t m_vsraw(VI_PARAMS) { VI_UNUSED; return vbin(OP_SRA, 4, 0, d, w, in, out); }
XTS_MODEL("vsraw", m_vsraw);

static xts_mres_t m_vaddcuw(VI_PARAMS) { VI_UNUSED; return vbin(OP_ADDC, 4, 0, d, w, in, out); }
XTS_MODEL("vaddcuw", m_vaddcuw);

static xts_mres_t m_vsubcuw(VI_PARAMS) { VI_UNUSED; return vbin(OP_SUBC, 4, 0, d, w, in, out); }
XTS_MODEL("vsubcuw", m_vsubcuw);

static xts_mres_t m_vrlw128(VI_PARAMS) { VI_UNUSED; return vbin(OP_RL, 4, 1, d, w, in, out); }
XTS_MODEL("vrlw128", m_vrlw128);

static xts_mres_t m_vslw128(VI_PARAMS) { VI_UNUSED; return vbin(OP_SL, 4, 1, d, w, in, out); }
XTS_MODEL("vslw128", m_vslw128);

static xts_mres_t m_vsraw128(VI_PARAMS) { VI_UNUSED; return vbin(OP_SRA, 4, 1, d, w, in, out); }
XTS_MODEL("vsraw128", m_vsraw128);

static xts_mres_t m_vsrw128(VI_PARAMS) { VI_UNUSED; return vbin(OP_SR, 4, 1, d, w, in, out); }
XTS_MODEL("vsrw128", m_vsrw128);

static xts_mres_t m_vcmpequw128(VI_PARAMS) { VI_UNUSED; return vbin(OP_EQ, 4, 1, d, w, in, out); }
XTS_MODEL("vcmpequw128", m_vcmpequw128);

static xts_mres_t m_vand(VI_PARAMS) { VI_UNUSED; return vlogic(OP_AND, 0, in, w, out); }
XTS_MODEL("vand", m_vand);

static xts_mres_t m_vand128(VI_PARAMS) { VI_UNUSED; return vlogic(OP_AND, 1, in, w, out); }
XTS_MODEL("vand128", m_vand128);

static xts_mres_t m_vandc(VI_PARAMS) { VI_UNUSED; return vlogic(OP_ANDC, 0, in, w, out); }
XTS_MODEL("vandc", m_vandc);

static xts_mres_t m_vandc128(VI_PARAMS) { VI_UNUSED; return vlogic(OP_ANDC, 1, in, w, out); }
XTS_MODEL("vandc128", m_vandc128);

static xts_mres_t m_vor(VI_PARAMS) { VI_UNUSED; return vlogic(OP_OR, 0, in, w, out); }
XTS_MODEL("vor", m_vor);

static xts_mres_t m_vor128(VI_PARAMS) { VI_UNUSED; return vlogic(OP_OR, 1, in, w, out); }
XTS_MODEL("vor128", m_vor128);

static xts_mres_t m_vxor(VI_PARAMS) { VI_UNUSED; return vlogic(OP_XOR, 0, in, w, out); }
XTS_MODEL("vxor", m_vxor);

static xts_mres_t m_vxor128(VI_PARAMS) { VI_UNUSED; return vlogic(OP_XOR, 1, in, w, out); }
XTS_MODEL("vxor128", m_vxor128);

static xts_mres_t m_vnor(VI_PARAMS) { VI_UNUSED; return vlogic(OP_NOR, 0, in, w, out); }
XTS_MODEL("vnor", m_vnor);

static xts_mres_t m_vnor128(VI_PARAMS) { VI_UNUSED; return vlogic(OP_NOR, 1, in, w, out); }
XTS_MODEL("vnor128", m_vnor128);

static xts_mres_t m_vspltb(VI_PARAMS) { VI_UNUSED; return vsplat(1, 0, w, in, out); }
XTS_MODEL("vspltb", m_vspltb);

static xts_mres_t m_vspltisb(VI_PARAMS) { VI_UNUSED; return vsplati(1, 0, w, out); }
XTS_MODEL("vspltisb", m_vspltisb);

static xts_mres_t m_vsplth(VI_PARAMS) { VI_UNUSED; return vsplat(2, 0, w, in, out); }
XTS_MODEL("vsplth", m_vsplth);

static xts_mres_t m_vspltish(VI_PARAMS) { VI_UNUSED; return vsplati(2, 0, w, out); }
XTS_MODEL("vspltish", m_vspltish);

static xts_mres_t m_vspltw(VI_PARAMS) { VI_UNUSED; return vsplat(4, 0, w, in, out); }
XTS_MODEL("vspltw", m_vspltw);

static xts_mres_t m_vspltisw(VI_PARAMS) { VI_UNUSED; return vsplati(4, 0, w, out); }
XTS_MODEL("vspltisw", m_vspltisw);

static xts_mres_t m_vspltw128(VI_PARAMS) { VI_UNUSED; return vsplat(4, 1, w, in, out); }
XTS_MODEL("vspltw128", m_vspltw128);

static xts_mres_t m_vspltisw128(VI_PARAMS) { VI_UNUSED; return vsplati(4, 1, w, out); }
XTS_MODEL("vspltisw128", m_vspltisw128);
