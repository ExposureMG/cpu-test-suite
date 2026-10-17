/* mfp: the PowerPC rounding path (also frsp vs f64_to_f32) (binary128 round-to-odd + xf_round) against SoftFloat's own f64/f32 operations on
 * random operands (all exponents incl. denormals, all four rounding modes): result bits and XX/OX/UX must agree. */
#include <string.h>
#include <xts/xts.h>
#include "softfloat.h"
#include "htest.h"

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
  rs ^= rs << 13;
  rs ^= rs >> 7;
  rs ^= rs << 17;
  return rs;
}
/* random finite double; exponents biased towards the edges */
static uint64_t rdouble(void)
{
  uint64_t r = rnd(), m = rnd() & 0xFFFFFFFFFFFFFull;
  uint64_t e;
  switch (r & 7) {
  case 0: e = 0; break;                                         /* zero / denormal */
  case 1: e = 1 + (r >> 8) % 60; break;
  case 2: e = 0x7FE - (r >> 8) % 60; break;
  case 3: m &= ~0xFFFFFFFFFull; e = 1023 - 30 + (r >> 8) % 60; break;   /* short significands */
  default: e = 1 + (r >> 8) % 0x7FE; break;
  }
  return (r & 0x8000000000000000ull) | (e << 52) | m;
}
/* random single (as float32 bits) */
static uint32_t rsingle(void)
{
  uint64_t r = rnd();
  uint32_t m = (uint32_t)(rnd() & 0x7FFFFF), e;
  switch (r & 3) {
  case 0: e = 1 + (r >> 8) % 20; break;
  case 1: e = 0xFE - (r >> 8) % 20; break;
  default: e = 1 + (r >> 8) % 0xFE; break;
  }
  return (uint32_t)(r >> 63) << 31 | e << 23 | m;
}

static xts_state_t S, O;
static xts_mres_t run(uint32_t w)
{
  xts_mem_t m;
  xts_mem_init(&m);
  return xts_model_exec(NULL, w, &S, &m, &O);
}
static uint32_t sf_exc(void)
{
  uint32_t f = 0, e = softfloat_exceptionFlags;
  if (e & softfloat_flag_overflow) f |= XTS_FPSCR_OX;
  if (e & softfloat_flag_underflow) f |= XTS_FPSCR_UX;
  if (e & softfloat_flag_inexact) f |= XTS_FPSCR_XX;
  return f;
}
#define EXC (XTS_FPSCR_OX | XTS_FPSCR_UX | XTS_FPSCR_XX)

HTEST(mfp_random_vs_softfloat)
{
  static const uint_fast8_t mode[4] = { softfloat_round_near_even, softfloat_round_minMag, softfloat_round_max,
                                        softfloat_round_min };
  int bad = 0, n = 0;
  for (int it = 0; it < 4000; it++) {
    for (int rn = 0; rn < 4; rn++) {
      for (int op = 0; op < 7; op++) {
        xts_state_baseline(&S);
        S.fpscr = (uint32_t)rn;
        float64_t a, b, c, r;
        a.v = rdouble(); b.v = rdouble(); c.v = rdouble();
        if (op == 5) {                                          /* fadds on single operands vs f32 */
          float32_t x, y;
          x.v = rsingle(); y.v = rsingle();
          a = f32_to_f64(x); b = f32_to_f64(y);
        }
        S.fpr[2] = a.v; S.fpr[3] = b.v; S.fpr[4] = c.v;
        softfloat_roundingMode = mode[rn];
        softfloat_exceptionFlags = 0;
        uint32_t w;
        switch (op) {
        case 0: r = f64_add(a, b); w = xts_enc(XI_faddx, 0, 1, 2, 3); break;
        case 1: r = f64_mul(a, c); w = xts_enc(XI_fmulx, 0, 1, 2, 4); break;
        case 2: r = f64_div(a, b); w = xts_enc(XI_fdivx, 0, 1, 2, 3); break;
        case 3: b.v &= ~0x8000000000000000ull; S.fpr[3] = b.v; r = f64_sqrt(b); w = xts_enc(XI_fsqrtx, 0, 1, 3); break;
        case 4: r = f64_mulAdd(a, c, b); w = xts_enc(XI_fmaddx, 0, 1, 2, 4, 3); break;
        case 6: {                                               /* frsp, incl. single denormals / overflow */
          a.v = (a.v & 0x800FFFFFFFFFFFFFull) | ((uint64_t)(0x380 - 40 + (rnd() % 400)) << 52);
          S.fpr[3] = a.v;
          r = f32_to_f64(f64_to_f32(a));
          w = xts_enc(XI_frspx, 0, 1, 3);
          break;
        }
        default: {
          float32_t x = f64_to_f32(a), y = f64_to_f32(b);
          softfloat_exceptionFlags = 0;
          r = f32_to_f64(f32_add(x, y));
          w = xts_enc(XI_faddsx, 0, 1, 2, 3);
          break;
        }
        }
        uint32_t want = sf_exc();
        if ((r.v & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull) continue;   /* NaN: covered elsewhere */
        if ((op == 2 && (b.v << 1) == 0)) continue;                           /* zero divide */
        n++;
        if (run(w) != XTS_M_OK || O.fpr[1] != r.v || (O.fpscr & EXC) != want) {
          if (bad++ < 5)
            printf("    op %d rn %d a %016llX b %016llX c %016llX: model %016llX %08X, softfloat %016llX %08X\n", op, rn,
                   (unsigned long long)a.v, (unsigned long long)b.v, (unsigned long long)c.v,
                   (unsigned long long)O.fpr[1], (unsigned)(O.fpscr & EXC), (unsigned long long)r.v, want);
        }
      }
    }
  }
  HT_CHECK(n > 90000);
  HT_EQ_U64(bad, 0);
}
