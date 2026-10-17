/* FP model core: exact binary128 intermediates, PowerPC rounding, FPSCR glue, operand access (owner: mfp).
 * See fp_core.h. Integer code + SoftFloat only. */
#include <string.h>
#include "fp_core.h"

/* ---- exact intermediate ---- */
xf_t xf_from_f128(float128_t f)
{
  xf_t x;
  uint64_t hi = f.v[XF_HI], lo = f.v[XF_LO];
  int be = (int)((hi >> 48) & 0x7FFF);
  x.sign = (int)(hi >> 63);
  x.zero = 0;
  if (be == 0) {
    /* zero; binary128 subnormals cannot come out of double-range operands */
    x.zero = 1;
    x.e = 0;
    x.mh = x.ml = 0;
    return x;
  }
  x.e = be - 16383;
  x.mh = (hi & 0xFFFFFFFFFFFFull) | (1ull << 48);
  x.ml = lo;
  return x;
}

xf_t xf_from_i64(int64_t v)
{
  xf_t x;
  memset(&x, 0, sizeof x);
  if (!v) { x.zero = 1; return x; }
  x.sign = v < 0;
  uint64_t m = x.sign ? 0 - (uint64_t)v : (uint64_t)v;
  int n = xf_bitlen(m) - 1;            /* leading bit position */
  x.e = n;
  /* place the leading bit at position 112 of mh:ml (bit 48 of mh) */
  int sh = 112 - n;                    /* 49..112 */
  if (sh >= 64) { x.mh = m << (sh - 64); x.ml = 0; }
  else { x.mh = m >> (64 - sh); x.ml = m << sh; }
  return x;
}

static float128_t to128(uint64_t d)
{
  float64_t f;
  f.v = d;
  return f64_to_f128(f);
}

xf_t xf_exact(int op, uint64_t a, uint64_t b, uint64_t c, int rn)
{
  uint_fast8_t saved_rm = softfloat_roundingMode;
  uint_fast8_t saved_fl = softfloat_exceptionFlags;
  softfloat_roundingMode = softfloat_round_odd;
  float128_t r, A = to128(a), B = to128(b);
  int zsign = 0;
  switch (op) {
  case XF_ADD: r = f128_add(A, B); zsign = xd_sign(a) == xd_sign(b) ? xd_sign(a) : rn == 3; break;
  case XF_MUL: r = f128_mul(A, B); zsign = xd_sign(a) ^ xd_sign(b); break;
  case XF_DIV: r = f128_div(A, B); zsign = xd_sign(a) ^ xd_sign(b); break;
  case XF_SQRT: r = f128_sqrt(A); zsign = xd_sign(a); break;
  default: {
    int sp = xd_sign(a) ^ xd_sign(b);
    r = f128_mulAdd(A, B, to128(c));
    zsign = sp == xd_sign(c) ? sp : rn == 3;
    break;
  }
  }
  softfloat_roundingMode = saved_rm;
  softfloat_exceptionFlags = saved_fl;
  xf_t x = xf_from_f128(r);
  if (x.zero) x.sign = zsign;          /* exact zero: IEEE sign rules with the real rounding mode */
  return x;
}

/* M >> d (d >= 1) with guard bit and sticky */
static void shr113(uint64_t mh, uint64_t ml, int d, uint64_t *R, int *g, int *s)
{
  if (d >= 128) { *R = 0; *g = 0; *s = (mh | ml) != 0; return; }
  if (d >= 64) *R = (d - 64) >= 64 ? 0 : mh >> (d - 64);
  else *R = (ml >> d) | (mh << (64 - d));
  int k = d - 1;
  if (k >= 64) {
    *g = (int)((mh >> (k - 64)) & 1);
    *s = ml != 0 || (k > 64 && (mh & ((1ull << (k - 64)) - 1)) != 0);
  } else {
    *g = (int)((ml >> k) & 1);
    *s = k > 0 && (ml & ((1ull << k) - 1)) != 0;
  }
}

/* sign * R * 2^q as a double (R < 2^54); q >= -1074 when the value is below the double normal range */
static uint64_t pack(int sign, uint64_t R, int q)
{
  uint64_t s = (uint64_t)sign << 63;
  if (!R) return s;
  int n = xf_bitlen(R) - 1;
  int E = q + n;
  if (E >= -1022) {
    uint64_t m = n <= 52 ? R << (52 - n) : R >> (n - 52);
    return s | ((uint64_t)(E + 1023) << 52) | (m & XD_FRAC);
  }
  return s | (R << (q + 1074));
}

xf_res_t xf_round(const xf_t *x, int prec, uint32_t fpscr)
{
  xf_res_t r;
  memset(&r, 0, sizeof r);
  int rn = (int)(fpscr & 3);
  if (x->zero) { r.bits = (uint64_t)x->sign << 63; return r; }
  const int emin = prec == 53 ? -1022 : -126, emax = prec == 53 ? 1023 : 127, adj = prec == 53 ? 1536 : 192;
  const int ue = (fpscr & XTS_FPSCR_UE) != 0, oe = (fpscr & XTS_FPSCR_OE) != 0;
  int tiny = x->e < emin;
  int d = 113 - prec;
  if (tiny && !ue) d += emin - x->e;   /* denormalise: round at the format's denormal position */
  uint64_t R;
  int g, s;
  shr113(x->mh, x->ml, d, &R, &g, &s);
  int q = x->e - 112 + d;
  int inexact = g | s, inc = 0;
  switch (rn) {
  case 0: inc = g && (s || (R & 1)); break;
  case 1: inc = 0; break;
  case 2: inc = !x->sign && inexact; break;
  default: inc = x->sign && inexact; break;
  }
  R += inc;
  r.fr = inc;
  r.fi = inexact;
  if (R >> prec) { R >>= 1; q++; }     /* carry out of the significand */
  if (!tiny && R && q + xf_bitlen(R) - 1 > emax) {
    r.exc |= XTS_FPSCR_OX;
    if (oe) {
      q -= adj;
      if (inexact) r.exc |= XTS_FPSCR_XX;
    } else {
      /* disabled overflow: XX, FI=1; FR is architecturally undefined, Xenon sets it (results4: frsp/fdivs RN=0,1) */
      r.exc |= XTS_FPSCR_XX;
      r.fi = 1;
      r.fr = 1;
      int to_max = rn == 1 || (rn == 2 && x->sign) || (rn == 3 && !x->sign);
      uint64_t mag = to_max ? (prec == 53 ? XD_MAXD : XD_MAXS) : XD_INF;
      r.bits = ((uint64_t)x->sign << 63) | mag;
      return r;
    }
  }
  if (tiny) {
    if (ue) {
      r.exc |= XTS_FPSCR_UX;
      q += adj;
      if (inexact) r.exc |= XTS_FPSCR_XX;
    } else if (inexact) {
      r.exc |= XTS_FPSCR_UX | XTS_FPSCR_XX;
    }
  } else if (inexact) {
    r.exc |= XTS_FPSCR_XX;
  }
  r.bits = pack(x->sign, R, q);
  return r;
}

uint32_t xf_fpscr_set(uint32_t fpscr, int fr, int fi, int64_t fprf, uint32_t exc)
{
  uint32_t f = fpscr & ~(XTS_FPSCR_FR | XTS_FPSCR_FI);
  if (fr) f |= XTS_FPSCR_FR;
  if (fi) f |= XTS_FPSCR_FI;
  if (fprf >= 0) f = (f & ~XTS_FPSCR_FPRF) | ((uint32_t)fprf & XTS_FPSCR_FPRF);
  return xm_fpscr_merge(f, exc);
}

int xf_op(const xts_insn_t *d, uint32_t w, const char *name)
{
  for (int i = 0; i < d->nops; i++)
    if (!strcmp(d->ops[i].name, name)) return (int)xts_extract(d->index, w, i);
  return -1;
}

/* ---- estimate accuracy check (recip / rsqrt only; see fp_core.h) ----
 * recip: |est*x - 1| <= 2^-k (est*x is exact in binary128); rsqrt: (1-2^-k)^2 <= est^2*x <= (1+2^-k)^2. */
int xf_estimate_ok(int kind, int vector, uint64_t x, uint64_t est)
{
  if (kind > 1) return -1;                       /* exp2/log2: no exact reference implemented */
  if (xd_isnan(x) || xd_isinf(x) || xd_iszero(x) || xd_isnan(est) || xd_isinf(est) || xd_iszero(est)) return -1;
  uint_fast8_t saved_rm = softfloat_roundingMode, saved_fl = softfloat_exceptionFlags;
  softfloat_roundingMode = softfloat_round_near_even;
  const int k = kind == 0 ? (vector ? 12 : 8) : (vector ? 12 : 5);
  float128_t X = to128(x), E = to128(est), one = i64_to_f128(1);
  float128_t b = one;
  b.v[XF_HI] -= (uint64_t)k << 48;               /* 2^-k */
  float128_t lo = f128_sub(one, b), hi = f128_add(one, b), p;
  if (kind == 0) {
    p = f128_mul(E, X);
  } else {
    p = f128_mul(f128_mul(E, E), X);
    lo = f128_mul(lo, lo);
    hi = f128_mul(hi, hi);
  }
  int ok = f128_le(lo, p) && f128_le(p, hi);
  softfloat_roundingMode = saved_rm;
  softfloat_exceptionFlags = saved_fl;
  return ok;
}
