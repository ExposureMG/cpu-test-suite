/* Scalar floating-point model (owner: mfp): arithmetic, multiply-add, rounding/conversion, compare, select, moves and
 * the FPSCR instructions. Full FPSCR semantics (FX, FEX, VX summary, VX*, OX UX ZX XX, FR FI, FPRF), RN, the enabled
 * exception actions of Book I 4.4 (target suppressed for enabled invalid / zero divide, exponent adjust for enabled
 * overflow / underflow; MSR[FE0,FE1] = 0 in our runs so no interrupt), Rc=1 -> CR1 = FPSCR[FX FEX VX OX].
 * Arithmetic goes through fp_core (SoftFloat binary128 round-to-odd + PowerPC rounding); no host FPU arithmetic.
 * Xenon facts (results4, KNOWN.md "## fp (mfp)"):
 *  - single-precision add/sub/mul/madd family: a finite nonzero operand outside the normal single range gives the default
 *    QNaN with no exception (after NaN, invalid and infinity handling); fdivs/fsqrts compute in double instead.
 *  - disabled overflow sets FR.
 *  - fctiw[z] results are sign-extended to 64 bits; FPRF is left unchanged by fcti* (architecturally undefined).
 *  - mffs returns 0xFFF80000 in the high word; FPSCR[NI] and the reserved bit 20 cannot be set. */
#include "fp_core.h"

#define XM_RC6(w) XM_BITS(w, 6, 5)     /* FRC */
#define FPCC 0x0000F000u

static uint64_t dnan(void) { return XM_DNAN64; }

static xts_mres_t done(const xts_insn_t *d, uint32_t w, xts_state_t *out)
{
  if (xf_rc(d, w)) xf_cr1(out);
  return XTS_M_OK;
}

/* FPRF of a single-precision result held in double format: single denormals classify as denormal */
static uint32_t fprf_single(uint64_t b)
{
  if (!xd_iszero(b) && !xd_isnan(b) && !xd_isinf(b) && xd_exp(b) < 897) return (xd_sign(b) ? 0x18u : 0x14u) << 12;
  return xm_fprf_f64(b);
}

/* finite nonzero double that is not a normal single (Xenon single-precision operand check) */
static int bad_single(uint64_t b)
{
  if (xd_isnan(b) || xd_isinf(b) || xd_iszero(b)) return 0;
  int e = xd_exp(b);
  return e < 897 || e > 1150;
}

enum { K_ADD, K_SUB, K_MUL, K_DIV, K_SQRT, K_MADD, K_MSUB, K_NMADD, K_NMSUB };

/* the arithmetic instructions: double (single = 0) or single precision */
static xts_mres_t arith(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int k, int single)
{
  const int t = XM_RD(w);
  const uint64_t A = in->fpr[XM_RA(w)], B = in->fpr[XM_RB(w)], C = in->fpr[XM_RC6(w)];
  const uint32_t fpscr = (uint32_t)in->fpscr;
  const int rn = (int)(fpscr & 3);
  const int neg = k == K_NMADD || k == K_NMSUB;
  uint64_t ord[3];                       /* NaN selection order (Book I 4.3.6: frA, frB, frC) */
  int n;
  uint64_t x, y, z = 0;                  /* semantic operands: x op y (FMA: x*y + z) */
  switch (k) {
  case K_ADD: ord[0] = A; ord[1] = B; n = 2; x = A; y = B; break;
  case K_SUB: ord[0] = A; ord[1] = B; n = 2; x = A; y = B ^ XD_SIGN; break;
  case K_MUL: ord[0] = A; ord[1] = C; n = 2; x = A; y = C; break;
  case K_DIV: ord[0] = A; ord[1] = B; n = 2; x = A; y = B; break;
  case K_SQRT: ord[0] = B; n = 1; x = B; y = 0; break;
  default:
    ord[0] = A; ord[1] = B; ord[2] = C; n = 3;
    x = A; y = C; z = (k == K_MSUB || k == K_NMSUB) ? B ^ XD_SIGN : B;
    break;
  }
  uint32_t vx = 0, zx = 0;
  uint64_t res = 0;
  int have = 0;                          /* 1: res is the final (pre-negation) result, no rounding needed */
  for (int i = 0; i < n; i++)
    if (xd_issnan(ord[i])) vx |= XTS_FPSCR_VXSNAN;
  for (int i = 0; i < n && !have; i++)
    if (xd_isnan(ord[i])) { res = xd_quiet(ord[i]); have = 2; }   /* 2: NaN (never negated) */
  /* multiply-add: inf*0 is flagged even when the addend is a NaN (results4: SNaN addend gives VXSNAN + VXIMZ) */
  if (have && k >= K_MADD && !xd_isnan(x) && !xd_isnan(y) &&
      ((xd_isinf(x) && xd_iszero(y)) || (xd_iszero(x) && xd_isinf(y))))
    vx |= XTS_FPSCR_VXIMZ;
  if (!have) {
    switch (k) {
    case K_ADD: case K_SUB:
      if (xd_isinf(x) && xd_isinf(y) && xd_sign(x) != xd_sign(y)) vx |= XTS_FPSCR_VXISI;
      else if (xd_isinf(x)) { res = x; have = 1; }
      else if (xd_isinf(y)) { res = y; have = 1; }
      break;
    case K_MUL:
      if ((xd_isinf(x) && xd_iszero(y)) || (xd_iszero(x) && xd_isinf(y))) vx |= XTS_FPSCR_VXIMZ;
      else if (xd_isinf(x) || xd_isinf(y)) { res = XD_INF | ((x ^ y) & XD_SIGN); have = 1; }
      break;
    case K_DIV:
      if (xd_isinf(x) && xd_isinf(y)) vx |= XTS_FPSCR_VXIDI;
      else if (xd_iszero(x) && xd_iszero(y)) vx |= XTS_FPSCR_VXZDZ;
      else if (xd_isinf(x)) { res = XD_INF | ((x ^ y) & XD_SIGN); have = 1; }
      else if (xd_isinf(y)) { res = (x ^ y) & XD_SIGN; have = 1; }
      else if (xd_iszero(y)) { zx = XTS_FPSCR_ZX; res = XD_INF | ((x ^ y) & XD_SIGN); have = 1; }
      break;
    case K_SQRT:
      if (xd_sign(x) && !xd_iszero(x)) vx |= XTS_FPSCR_VXSQRT;
      else if (xd_isinf(x) || xd_iszero(x)) { res = x; have = 1; }
      break;
    default: {
      int sp = xd_sign(x) ^ xd_sign(y);
      if ((xd_isinf(x) && xd_iszero(y)) || (xd_iszero(x) && xd_isinf(y))) vx |= XTS_FPSCR_VXIMZ;
      else if (xd_isinf(x) || xd_isinf(y)) {
        if (xd_isinf(z) && xd_sign(z) != sp) vx |= XTS_FPSCR_VXISI;
        else { res = XD_INF | ((uint64_t)sp << 63); have = 1; }
      } else if (xd_isinf(z)) { res = z; have = 1; }
      break;
    }
    }
    if (vx) { res = dnan(); have = 2; }
  }
  int prec = single ? 24 : 53;
  if (single && !have) {
    int bad = 0;
    for (int i = 0; i < n; i++) bad |= bad_single(ord[i]);
    if (bad) {
      if (k == K_DIV || k == K_SQRT) prec = 53;          /* Xenon: computed in double */
      else { res = dnan(); have = 2; }                    /* Xenon: default QNaN, no exception */
    }
  }
  /* enabled invalid / zero divide: target and FPRF unchanged, FR FI cleared */
  if ((vx && (fpscr & XTS_FPSCR_VE)) || (zx && (fpscr & XTS_FPSCR_ZE))) {
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, -1, vx | zx);
    return done(d, w, out);
  }
  if (have) {
    if (have == 1 && neg) res ^= XD_SIGN;
    out->fpr[t] = res;
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, single ? fprf_single(res) : xm_fprf_f64(res), vx | zx);
    return done(d, w, out);
  }
  xf_t e;
  switch (k) {
  case K_ADD: case K_SUB: e = xf_exact(XF_ADD, x, y, 0, rn); break;
  case K_MUL: e = xf_exact(XF_MUL, x, y, 0, rn); break;
  case K_DIV: e = xf_exact(XF_DIV, x, y, 0, rn); break;
  case K_SQRT: e = xf_exact(XF_SQRT, x, 0, 0, rn); break;
  default: e = xf_exact(XF_FMA, x, y, z, rn); break;
  }
  xf_res_t r = xf_round(&e, prec, fpscr);
  if (neg) r.bits ^= XD_SIGN;
  out->fpr[t] = r.bits;
  out->fpscr = xf_fpscr_set(fpscr, r.fr, r.fi, prec == 24 ? fprf_single(r.bits) : xm_fprf_f64(r.bits), r.exc);
  return done(d, w, out);
}

#define ARITH(name, fn, k, s)                                                                                   \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return arith(d, w, in, out, k, s);                                                                          \
  }                                                                                                             \
  XTS_MODEL(name, fn)

ARITH("faddx", m_fadd, K_ADD, 0);
ARITH("fsubx", m_fsub, K_SUB, 0);
ARITH("fmulx", m_fmul, K_MUL, 0);
ARITH("fdivx", m_fdiv, K_DIV, 0);
ARITH("fsqrtx", m_fsqrt, K_SQRT, 0);
ARITH("fmaddx", m_fmadd, K_MADD, 0);
ARITH("fmsubx", m_fmsub, K_MSUB, 0);
ARITH("fnmaddx", m_fnmadd, K_NMADD, 0);
ARITH("fnmsubx", m_fnmsub, K_NMSUB, 0);
ARITH("faddsx", m_fadds, K_ADD, 1);
ARITH("fsubsx", m_fsubs, K_SUB, 1);
ARITH("fmulsx", m_fmuls, K_MUL, 1);
ARITH("fdivsx", m_fdivs, K_DIV, 1);
ARITH("fsqrtsx", m_fsqrts, K_SQRT, 1);
ARITH("fmaddsx", m_fmadds, K_MADD, 1);
ARITH("fmsubsx", m_fmsubs, K_MSUB, 1);
ARITH("fnmaddsx", m_fnmadds, K_NMADD, 1);
ARITH("fnmsubsx", m_fnmsubs, K_NMSUB, 1);

/* frsp[.] FRT,FRB (Book I A.1) */
static xts_mres_t m_frsp(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  const uint64_t B = in->fpr[XM_RB(w)];
  const uint32_t fpscr = (uint32_t)in->fpscr;
  const int t = XM_RD(w);
  if (xd_isnan(B)) {
    uint32_t vx = xd_issnan(B) ? XTS_FPSCR_VXSNAN : 0;
    if (vx && (fpscr & XTS_FPSCR_VE)) { out->fpscr = xf_fpscr_set(fpscr, 0, 0, -1, vx); return done(d, w, out); }
    uint64_t r = xd_quiet(B) & ~0x1FFFFFFFull;          /* FRB0:34 || 29 zeros, quieted */
    out->fpr[t] = r;
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, xm_fprf_f64(r), vx);
    return done(d, w, out);
  }
  if (xd_isinf(B) || xd_iszero(B)) {
    out->fpr[t] = B;
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, xm_fprf_f64(B), 0);
    return done(d, w, out);
  }
  float64_t f;
  f.v = B;
  xf_t e = xf_from_f128(f64_to_f128(f));
  xf_res_t r = xf_round(&e, 24, fpscr);
  out->fpr[t] = r.bits;
  out->fpscr = xf_fpscr_set(fpscr, r.fr, r.fi, fprf_single(r.bits), r.exc);
  return done(d, w, out);
}
XTS_MODEL("frspx", m_frsp);

/* fcti{w,d}[z][.] FRT,FRB (Book I A.2) */
static xts_mres_t cvt_int(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int dw, int rz)
{
  const uint64_t B = in->fpr[XM_RB(w)];
  const uint32_t fpscr = (uint32_t)in->fpscr;
  const int rn = rz ? 1 : (int)(fpscr & 3), sign = xd_sign(B), t = XM_RD(w);
  const uint64_t imax = dw ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
  const uint64_t imin = dw ? 0x8000000000000000ull : 0xFFFFFFFF80000000ull;   /* fctiw: sign-extended (Xenon) */
  uint32_t vx = 0;
  uint64_t res = 0;
  int fr = 0, fi = 0;
  if (xd_isnan(B)) {
    vx = XTS_FPSCR_VXCVI | (xd_issnan(B) ? XTS_FPSCR_VXSNAN : 0);
    res = imin;
  } else if (xd_isinf(B)) {
    vx = XTS_FPSCR_VXCVI;
    res = sign ? imin : imax;
  } else {
    int be = xd_exp(B);
    int e = be ? be - 1023 : -1022;
    uint64_t mnt = (B & XD_FRAC) | (be ? 1ull << 52 : 0);
    uint64_t mag;
    int g = 0, s = 0, big = 0;
    if (e > 63) big = 1;
    if (big) {
      mag = 0;
    } else if (e >= 52) {
      mag = mnt << (e - 52);
    } else {
      int sh = 52 - e;                   /* >= 1 */
      if (sh > 64) { mag = 0; g = 0; s = mnt != 0; }
      else if (sh == 64) { mag = 0; g = (int)(mnt >> 63); s = (mnt & 0x7FFFFFFFFFFFFFFFull) != 0; }
      else {
        mag = mnt >> sh;
        g = (int)((mnt >> (sh - 1)) & 1);
        s = (mnt & ((1ull << (sh - 1)) - 1)) != 0;
      }
    }
    int inexact = g | s, inc = 0;
    switch (rn) {
    case 0: inc = g && (s || (mag & 1)); break;
    case 1: inc = 0; break;
    case 2: inc = !sign && inexact; break;
    default: inc = sign && inexact; break;
    }
    if (!big) {
      uint64_t m2 = mag + (uint64_t)inc;
      if (m2 < mag) big = 1;             /* carry out of 64 bits */
      mag = m2;
    }
    uint64_t lim = sign ? (dw ? 0x8000000000000000ull : 0x80000000ull) : (dw ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull);
    if (big || mag > lim) {
      vx = XTS_FPSCR_VXCVI;
      res = sign ? imin : imax;
    } else {
      uint64_t v = sign ? 0 - mag : mag;
      res = dw ? v : (uint64_t)(int64_t)(int32_t)(uint32_t)v;
      fr = inc;
      fi = inexact;
    }
  }
  if (vx) {
    if (!(fpscr & XTS_FPSCR_VE)) out->fpr[t] = res;
    out->fpscr = xf_fpscr_set(fpscr, 0, 0, -1, vx);
  } else {
    out->fpr[t] = res;
    out->fpscr = xf_fpscr_set(fpscr, fr, fi, -1, fi ? XTS_FPSCR_XX : 0);
  }
  return done(d, w, out);
}
#define CVT(name, fn, dw, rz)                                                                                   \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    return cvt_int(d, w, in, out, dw, rz);                                                                      \
  }                                                                                                             \
  XTS_MODEL(name, fn)
CVT("fctiwx", m_fctiw, 0, 0);
CVT("fctiwzx", m_fctiwz, 0, 1);
CVT("fctidx", m_fctid, 1, 0);
CVT("fctidzx", m_fctidz, 1, 1);

/* fcfid[.] FRT,FRB */
static xts_mres_t m_fcfid(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  const uint32_t fpscr = (uint32_t)in->fpscr;
  xf_t e = xf_from_i64((int64_t)in->fpr[XM_RB(w)]);
  xf_res_t r = xf_round(&e, 53, fpscr);
  out->fpr[XM_RD(w)] = r.bits;
  out->fpscr = xf_fpscr_set(fpscr, r.fr, r.fi, xm_fprf_f64(r.bits), r.exc);
  return done(d, w, out);
}
XTS_MODEL("fcfidx", m_fcfid);

/* compare: 8 LT, 4 GT, 2 EQ, 1 unordered */
static uint32_t fcmp_code(uint64_t a, uint64_t b)
{
  if (xd_isnan(a) || xd_isnan(b)) return 1;
  if (xd_iszero(a) && xd_iszero(b)) return 2;
  /* sign-magnitude -> ordered integer key */
  int64_t ka = xd_sign(a) ? -(int64_t)(a & ~XD_SIGN) : (int64_t)a;
  int64_t kb = xd_sign(b) ? -(int64_t)(b & ~XD_SIGN) : (int64_t)b;
  return ka < kb ? 8 : ka > kb ? 4 : 2;
}

static xts_mres_t fcmp(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_state_t *out, int ordered)
{
  (void)d;
  const uint64_t A = in->fpr[XM_RA(w)], B = in->fpr[XM_RB(w)];
  const uint32_t fpscr = (uint32_t)in->fpscr;
  uint32_t c = fcmp_code(A, B), vx = 0;
  int snan = xd_issnan(A) || xd_issnan(B);
  if (snan) {
    vx |= XTS_FPSCR_VXSNAN;
    if (ordered && !(fpscr & XTS_FPSCR_VE)) vx |= XTS_FPSCR_VXVC;
  } else if (ordered && c == 1) {
    vx |= XTS_FPSCR_VXVC;
  }
  uint32_t f = (fpscr & ~FPCC) | (c << 12);            /* FR FI C unchanged */
  out->fpscr = xm_fpscr_merge(f, vx);
  xm_set_cr_field(out, (int)XM_BITS(w, 23, 3), c);
  return XTS_M_OK;
}
static xts_mres_t m_fcmpu(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  return fcmp(d, w, in, out, 0);
}
XTS_MODEL("fcmpu", m_fcmpu);
static xts_mres_t m_fcmpo(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  return fcmp(d, w, in, out, 1);
}
XTS_MODEL("fcmpo", m_fcmpo);

/* fsel[.] FRT,FRA,FRC,FRB: FRA >= 0 (-0 included, NaN excluded) ? FRC : FRB; no FPSCR change */
static xts_mres_t m_fsel(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  uint64_t A = in->fpr[XM_RA(w)];
  int ge = !xd_isnan(A) && (!xd_sign(A) || xd_iszero(A));
  out->fpr[XM_RD(w)] = ge ? in->fpr[XM_RC6(w)] : in->fpr[XM_RB(w)];
  return done(d, w, out);
}
XTS_MODEL("fselx", m_fsel);

/* moves: bit operations, no FPSCR change */
#define FMOVE(name, fn, expr)                                                                                   \
  static xts_mres_t fn(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out) \
  {                                                                                                             \
    (void)m;                                                                                                    \
    uint64_t b = in->fpr[XM_RB(w)];                                                                             \
    out->fpr[XM_RD(w)] = (expr);                                                                                \
    return done(d, w, out);                                                                                     \
  }                                                                                                             \
  XTS_MODEL(name, fn)
FMOVE("fmrx", m_fmr, b);
FMOVE("fnegx", m_fneg, b ^ XD_SIGN);
FMOVE("fabsx", m_fabs, b & ~XD_SIGN);
FMOVE("fnabsx", m_fnabs, b | XD_SIGN);

/* ---- FPSCR instructions ---- */
#define FPSCR_EXC_BITS (XTS_FPSCR_FX | XTS_FPSCR_OX | XTS_FPSCR_UX | XTS_FPSCR_ZX | XTS_FPSCR_XX | XTS_FPSCR_VX_ALL)
#define FPSCR_UNSETTABLE (XTS_FPSCR_NI | XTS_FPSCR_RSVD20)   /* Xenon: neither can be set (xm_fpscr_loaded) */

/* FEX/VX are summaries; FX as given */
static uint32_t fpscr_settle(uint32_t f) { return xm_fpscr_merge(f & ~FPSCR_UNSETTABLE, 0); }

/* mffs[.] FRT */
static xts_mres_t m_mffs(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  out->fpr[XM_RD(w)] = 0xFFF8000000000000ull | (uint32_t)in->fpscr;
  return done(d, w, out);
}
XTS_MODEL("mffsx", m_mffs);

/* mcrfs BF,BFA: copy the field, clear the exception bits copied */
static xts_mres_t m_mcrfs(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  int bf = (int)XM_BITS(w, 23, 3), bfa = (int)XM_BITS(w, 18, 3);
  uint32_t f = (uint32_t)in->fpscr, sh = 28 - 4 * (uint32_t)bfa;
  xm_set_cr_field(out, bf, (f >> sh) & 0xF);
  f &= ~((0xFu << sh) & FPSCR_EXC_BITS);
  out->fpscr = xm_fpscr_merge(f, 0);
  return XTS_M_OK;
}
XTS_MODEL("mcrfs", m_mcrfs);

/* mtfsfi[.] BF,U */
static xts_mres_t m_mtfsfi(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  int bf = (int)XM_BITS(w, 23, 3);
  uint32_t u = XM_BITS(w, 12, 4), sh = 28 - 4 * (uint32_t)bf;
  uint32_t f = ((uint32_t)in->fpscr & ~(0xFu << sh)) | (u << sh);
  out->fpscr = fpscr_settle(f);
  return done(d, w, out);
}
XTS_MODEL("mtfsfix", m_mtfsfi);

/* mtfsf[.] FLM,FRB */
static xts_mres_t m_mtfsf(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  uint32_t flm = XM_BITS(w, 17, 8), mask = 0;
  for (int i = 0; i < 8; i++)
    if (flm & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
  uint32_t f = ((uint32_t)in->fpscr & ~mask) | ((uint32_t)in->fpr[XM_RB(w)] & mask);
  out->fpscr = fpscr_settle(f);
  return done(d, w, out);
}
XTS_MODEL("mtfsfx", m_mtfsf);

/* mtfsb0[.] BT */
static xts_mres_t m_mtfsb0(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  uint32_t bit = 0x80000000u >> XM_RD(w);
  out->fpscr = fpscr_settle((uint32_t)in->fpscr & ~bit);
  return done(d, w, out);
}
XTS_MODEL("mtfsb0x", m_mtfsb0);

/* mtfsb1[.] BT: FX is set when an exception bit goes 0 -> 1 (results4: OX, UX, XX, VXSNAN, VXISI, VXIDI, VXZDZ,
 * VXIMZ, VXVC, VXSQRT, VXCVI; not for FPRF/enable bits); FEX and VX cannot be set explicitly */
static xts_mres_t m_mtfsb1(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  uint32_t bit = 0x80000000u >> XM_RD(w), f = (uint32_t)in->fpscr;
  if (bit & (XTS_FPSCR_FEX | XTS_FPSCR_VX)) bit = 0;
  if (bit & (FPSCR_EXC_BITS & ~XTS_FPSCR_FX)) f = xm_fpscr_merge(f, bit);
  else f |= bit;
  out->fpscr = fpscr_settle(f);
  return done(d, w, out);
}
XTS_MODEL("mtfsb1x", m_mtfsb1);
