/* Vector estimate instructions vrefp vrsqrtefp vexptefp vlogefp and their VMX128 forms (owner: mfp).
 * Only results the architecture fixes are modelled: the PEM special-value tables, vexptefp of an integral input (exact
 * unless the result is 0 or +inf), vlogefp of 2^y (exact, y in [-149, 127]), and results whose magnitude makes the
 * value certain (overflow to infinity, flush to zero). vexptefp flushes denormal RESULTS to +0 whatever VSCR[NJ] is
 * (console run, Jasper). If any lane needs the hardware table the handler returns XTS_M_UNIMPLEMENTED (semantic check:
 * skip; KNOWN.md "## fp (mfp)"). results4 (24 cases, 96 lanes per op) shows the
 * legacy and VMX128 forms agree on every lane they share and SNaN -> QNaN with the payload kept. */
#include "fp_core.h"
#include "vfp.h"

enum { E_RECIP, E_RSQRT, E_EXP2, E_LOG2 };

/* 1 if the lane result is known (written to *r) */
static int lane_est(int k, uint32_t x, int nj, uint32_t *r)
{
  x = xv_nj(x, nj);
  const uint32_t s = x & XS_SIGN, mag = x & ~XS_SIGN;
  const int be = (int)(mag >> 23);
  if (xs_isnan(x)) { *r = x | XS_QBIT; return 1; }
  switch (k) {
  case E_RECIP:
    if (mag == 0) { *r = s | XS_INF; return 1; }
    if (mag == XS_INF) { *r = s; return 1; }
    if (mag < 0x00100000u) { *r = s | XS_INF; return 1; }       /* |x| < 2^-129: 1/x > 2^129 even with 2^-12 error */
    if (nj && be >= 127 + 127) { *r = s; return 1; }             /* |x| >= 2^127: result < 2^-126 -> flushed */
    return 0;
  case E_RSQRT:
    if (mag == 0) { *r = s | XS_INF; return 1; }
    if (s) { *r = 0x7FC00000u; return 1; }
    if (mag == XS_INF) { *r = 0; return 1; }
    return 0;
  case E_EXP2: {
    if (mag == XS_INF) { *r = s ? 0 : XS_INF; return 1; }
    if (mag == 0) { *r = 0x3F800000u; return 1; }
    if (!s && be < 127 - 100) { *r = 0x3F800000u; return 1; }   /* 0 < x < 2^-100 (results4: denormals, 2^-126) */
    /* Xenon flushes every denormal vexptefp result to +0, also with VSCR[NJ]=0 (console run, Jasper: 2^-127 .. 2^-149
     * integral and non-integral inputs all give 0, 2^-126 is still exact). x <= -127 puts the result below 2^-126 by
     * more than the estimate error (2^-12), so the flush is certain; (-127, -126) is left to the hardware table. */
    if (s && mag >= 0x42FE0000u) { *r = 0; return 1; }           /* x <= -127 */
    /* integral x? */
    if (be < 127) return 0;                                      /* 0 < |x| < 1: not integral */
    int e = be - 127;
    if (e < 23 && (mag & ((1u << (23 - e)) - 1))) return 0;      /* fraction bits set */
    if (e >= 8) { *r = s ? 0 : XS_INF; return 1; }               /* |x| >= 256 */
    int32_t n = (int32_t)(((mag & 0x7FFFFFu) | 0x800000u) >> (23 - e));
    if (s) n = -n;
    if (n >= 128) { *r = XS_INF; return 1; }
    if (n <= -150) { *r = 0; return 1; }
    if (n >= -126) { *r = (uint32_t)(n + 127) << 23; return 1; }
    *r = 0;                                                      /* denormal 2^n (n < -126): flushed to +0 */
    return 1;
  }
  default:  /* E_LOG2 */
    if (mag == 0) { *r = 0xFF800000u; return 1; }
    if (s) { *r = 0x7FC00000u; return 1; }
    if (mag == XS_INF) { *r = XS_INF; return 1; }
    int y;
    if (be) {
      if (mag & 0x7FFFFFu) return 0;
      y = be - 127;
    } else {
      if (mag & (mag - 1)) return 0;                             /* denormal power of two (NJ=0) */
      y = xf_bitlen(mag) - 1 - 149;
    }
    xv_sf_begin();
    *r = i32_to_f32(y).v;
    return 1;
  }
}

static xts_mres_t vest(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int k)
{
  vops_t o = xv_ops(d, w);
  int nj = xv_njmode(in);
  xts_vr_t R;
  for (int i = 0; i < 4; i++)
    if (!lane_est(k, in->vr[o.vb].w[i], nj, &R.w[i])) return XTS_M_UNIMPLEMENTED;
  out->vr[o.vd] = R;
  return XTS_M_OK;
}
#define VEST(name, fn, k)                                                                                       \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return vest(d, w, in, out, k);                                                                              \
  }                                                                                                             \
  XTS_MODEL(name, fn)
VEST("vrefp", m_vrefp, E_RECIP);
VEST("vrsqrtefp", m_vrsqrtefp, E_RSQRT);
VEST("vexptefp", m_vexptefp, E_EXP2);
VEST("vlogefp", m_vlogefp, E_LOG2);
VEST("vrefp128", m_vrefp128, E_RECIP);
VEST("vrsqrtefp128", m_vrsqrtefp128, E_RSQRT);
VEST("vexptefp128", m_vexptefp128, E_EXP2);
VEST("vlogefp128", m_vlogefp128, E_LOG2);
