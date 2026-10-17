/* Scalar estimate instructions fres[.] and frsqrte[.] (owner: mfp).
 * The Xenon estimate tables cannot be derived from results4 (24 cases each, 5-9 distinct normal inputs), so only the
 * architected special cases are modelled exactly; any input whose result needs the table returns XTS_M_UNIMPLEMENTED
 * (the semantic check records it as skip and keeps the CPU result). See KNOWN.md "## fp (mfp)".
 * Observed (results4): results carry 8 significant bits (fres) / 9 (frsqrte), are independent of RN, and leave
 * FR/FI/XX clear; fres overflows at the single range with OX only (no XX) and flushes |x| >= 2^128 to zero without UX. */
#include "fp_core.h"

static xts_mres_t done(const xts_insn_t *d, uint32_t w, xts_state_t *out)
{
  if (xf_rc(d, w)) xf_cr1(out);
  return XTS_M_OK;
}

/* common specials; returns 1 when handled */
static int est_special(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int rsqrt)
{
  const uint64_t B = in->fpr[XM_RB(w)];
  const uint32_t fpscr = (uint32_t)in->fpscr;
  const int t = XM_RD(w);
  uint32_t vx = 0, zx = 0;
  uint64_t res;
  if (xd_isnan(B)) {
    if (xd_issnan(B)) vx = XTS_FPSCR_VXSNAN;
    res = xd_quiet(B);
  } else if (xd_iszero(B)) {
    zx = XTS_FPSCR_ZX;
    res = XD_INF | (B & XD_SIGN);
  } else if (rsqrt && xd_sign(B)) {
    vx = XTS_FPSCR_VXSQRT;
    res = XM_DNAN64;
  } else if (xd_isinf(B)) {
    res = B & XD_SIGN;
  } else {
    return 0;
  }
  if ((vx && (fpscr & XTS_FPSCR_VE)) || (zx && (fpscr & XTS_FPSCR_ZE))) {
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, -1, vx | zx);
  } else {
    out->fpr[t] = res;
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, xm_fprf_f64(res), vx | zx);
  }
  done(d, w, out);
  return 1;
}

/* fres[.] FRT,FRB */
static xts_mres_t m_fres(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  if (est_special(d, w, in, out, 0)) return XTS_M_OK;
  const uint64_t B = in->fpr[XM_RB(w)];
  const uint32_t fpscr = (uint32_t)in->fpscr;
  const int s = xd_sign(B), be = xd_exp(B), rn = (int)(fpscr & 3);
  int e = be ? be - 1023 : xf_bitlen(B & XD_FRAC) - 1 - 1074;   /* exponent of the leading bit */
  if (e <= -130 && !(fpscr & XTS_FPSCR_OE)) {
    /* 1/x > 2^129: overflow at the single range; results4: OX without XX, FR FI clear, value by RN */
    int to_max = rn == 1 || (rn == 2 && s) || (rn == 3 && !s);
    uint64_t r = ((uint64_t)s << 63) | (to_max ? XD_MAXS : XD_INF);
    out->fpr[XM_RD(w)] = r;
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, xm_fprf_f64(r), XTS_FPSCR_OX);
    return done(d, w, out);
  }
  if (e >= 128 && !(fpscr & XTS_FPSCR_UE)) {
    /* 1/x < 2^-128: results4 (x ~ 2^995, RN 0/2/3) gives a signed zero with no UX/XX */
    uint64_t r = (uint64_t)s << 63;
    out->fpr[XM_RD(w)] = r;
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, xm_fprf_f64(r), 0);
    return done(d, w, out);
  }
  return XTS_M_UNIMPLEMENTED;          /* needs the hardware table */
}
XTS_MODEL("fresx", m_fres);

/* frsqrte[.] FRT,FRB */
static xts_mres_t m_frsqrte(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  if (est_special(d, w, in, out, 1)) return XTS_M_OK;
  return XTS_M_UNIMPLEMENTED;          /* positive finite: needs the hardware table */
}
XTS_MODEL("frsqrtex", m_frsqrte);
