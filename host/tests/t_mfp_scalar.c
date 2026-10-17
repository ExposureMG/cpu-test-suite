/* mfp: scalar FP model - special values, NaN propagation, denormals, rounding modes, enabled exceptions, FPSCR ops */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

#define FX XTS_FPSCR_FX
#define FEX XTS_FPSCR_FEX
#define VX XTS_FPSCR_VX
#define FPRF(c) ((uint32_t)(c) << 12)

static xts_state_t S, O;

static xts_mres_t run(uint32_t w)
{
  xts_mem_t m;
  xts_mem_init(&m);
  return xts_model_exec(NULL, w, &S, &m, &O);
}
static void reset(uint32_t fpscr)
{
  xts_state_baseline(&S);
  S.fpscr = fpscr;
}
/* FRT=1, FRA=2, FRB=3, FRC=4 */
static void ab(uint64_t a, uint64_t b) { S.fpr[2] = a; S.fpr[3] = b; }

HTEST(mfp_round_modes)
{
  static const uint64_t want[4] = { 0x3FD3333333333334ull, 0x3FD3333333333333ull, 0x3FD3333333333334ull,
                                    0x3FD3333333333333ull };
  for (uint32_t rn = 0; rn < 4; rn++) {
    reset(rn);
    ab(0x3FB999999999999Aull, 0x3FC999999999999Aull);         /* 0.1 + 0.2 */
    HT_EQ_U64(run(xts_enc(XI_faddx, 0, 1, 2, 3)), XTS_M_OK);
    HT_EQ_U64(O.fpr[1], want[rn]);
    uint32_t fr = (rn == 0 || rn == 2) ? XTS_FPSCR_FR : 0;
    HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_XX | XTS_FPSCR_FI | fr | FPRF(0x04) | rn);
  }
  /* exact zero sums: +0 except RN=3 */
  reset(0);
  ab(0x3FF0000000000000ull, 0xBFF0000000000000ull);
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0);
  HT_EQ_U64(O.fpscr, FPRF(0x02));
  reset(3);
  ab(0x3FF0000000000000ull, 0xBFF0000000000000ull);
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x8000000000000000ull);
  reset(0);
  ab(0x8000000000000000ull, 0x8000000000000000ull);              /* -0 + -0 = -0 */
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x8000000000000000ull);
  HT_EQ_U64(O.fpscr, FPRF(0x12));
  /* frsp 0.1 */
  reset(0);
  S.fpr[3] = 0x3FB999999999999Aull;
  run(xts_enc(XI_frspx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0x3FB99999A0000000ull);
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_XX | XTS_FPSCR_FR | XTS_FPSCR_FI | FPRF(0x04));
}

HTEST(mfp_nan_invalid)
{
  /* first NaN operand wins, quieted, payload kept; SNaN anywhere sets VXSNAN */
  reset(0);
  ab(0x7FF8000000000123ull, 0x7FF0000000000001ull);
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x7FF8000000000123ull);
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXSNAN | FPRF(0x11));
  reset(0);
  ab(0x3FF0000000000000ull, 0x7FF0000000000001ull);
  run(xts_enc(XI_fsubx, XTS_RC, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x7FF8000000000001ull);
  HT_EQ_U64((uint32_t)O.cr >> 24 & 0xF, 0xA);                    /* CR1 = FX FEX VX OX = 1010 */
  /* fmadd NaN order frA, frB, frC (frB is the addend) */
  reset(0);
  S.fpr[2] = 0x3FF0000000000000ull;
  S.fpr[3] = 0x7FF800000000000Bull;
  S.fpr[4] = 0x7FF800000000000Cull;
  run(xts_enc(XI_fmaddx, 0, 1, 2, 4, 3));
  HT_EQ_U64(O.fpr[1], 0x7FF800000000000Bull);
  /* fnmadd does not negate a NaN */
  run(xts_enc(XI_fnmaddx, 0, 1, 2, 4, 3));
  HT_EQ_U64(O.fpr[1], 0x7FF800000000000Bull);
  /* inf - inf, 0 * inf, 0/0, inf/inf, sqrt(-1) */
  reset(0);
  ab(0x7FF0000000000000ull, 0x7FF0000000000000ull);
  run(xts_enc(XI_fsubx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], XM_DNAN64);
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXISI | FPRF(0x11));
  run(xts_enc(XI_fdivx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXIDI | FPRF(0x11));
  reset(0);
  ab(0, 0);
  run(xts_enc(XI_fdivx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXZDZ | FPRF(0x11));
  reset(0);
  S.fpr[2] = 0;
  S.fpr[4] = 0xFFF0000000000000ull;
  run(xts_enc(XI_fmulx, 0, 1, 2, 4));
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXIMZ | FPRF(0x11));
  reset(0);
  S.fpr[3] = 0xBFF0000000000000ull;
  run(xts_enc(XI_fsqrtx, 0, 1, 3));
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXSQRT | FPRF(0x11));
  S.fpr[3] = 0x8000000000000000ull;                              /* sqrt(-0) = -0, no exception */
  run(xts_enc(XI_fsqrtx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0x8000000000000000ull);
  HT_EQ_U64(O.fpscr, FPRF(0x12));
  /* enabled invalid: target and FPRF unchanged, FEX set */
  reset(XTS_FPSCR_VE | FPRF(0x04));
  ab(0x3FF0000000000000ull, 0x7FF0000000000001ull);
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], S.fpr[1]);
  HT_EQ_U64(O.fpscr, FX | FEX | VX | XTS_FPSCR_VXSNAN | XTS_FPSCR_VE | FPRF(0x04));
  /* zero divide disabled / enabled */
  reset(0);
  ab(0xC000000000000000ull, 0);
  run(xts_enc(XI_fdivx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0xFFF0000000000000ull);
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_ZX | FPRF(0x09));
  reset(XTS_FPSCR_ZE);
  ab(0xC000000000000000ull, 0);
  run(xts_enc(XI_fdivx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], S.fpr[1]);
  HT_EQ_U64(O.fpscr, FX | FEX | XTS_FPSCR_ZX | XTS_FPSCR_ZE);
  /* compares */
  reset(0);
  ab(0x7FF8000000000000ull, 0x3FF0000000000000ull);
  run(xts_enc(XI_fcmpo, 0, 3, 2, 3));
  HT_EQ_U64(xm_cr_field(&O, 3), 1);
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXVC | FPRF(0x01));
  run(xts_enc(XI_fcmpu, 0, 3, 2, 3));
  HT_EQ_U64(O.fpscr, FPRF(0x01));
  ab(0x8000000000000000ull, 0);                                   /* -0 == +0 */
  run(xts_enc(XI_fcmpu, 0, 7, 2, 3));
  HT_EQ_U64(xm_cr_field(&O, 7), 2);
}

HTEST(mfp_denormal_overflow)
{
  /* exact denormal product: no UX */
  reset(0);
  S.fpr[2] = 0x0010000000000000ull;
  S.fpr[4] = 0x3FE0000000000000ull;
  run(xts_enc(XI_fmulx, 0, 1, 2, 4));
  HT_EQ_U64(O.fpr[1], 0x0008000000000000ull);
  HT_EQ_U64(O.fpscr, FPRF(0x14));
  /* 2^-1074 / 2: tie to even -> +0, UX XX FI */
  reset(0);
  ab(1, 0x4000000000000000ull);
  run(xts_enc(XI_fdivx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0);
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_UX | XTS_FPSCR_XX | XTS_FPSCR_FI | FPRF(0x02));
  /* same with UE=1: exponent + 1536, exact so no XX */
  reset(XTS_FPSCR_UE);
  ab(1, 0x4000000000000000ull);
  run(xts_enc(XI_fdivx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x5CC0000000000000ull);
  HT_EQ_U64(O.fpscr, FX | FEX | XTS_FPSCR_UX | XTS_FPSCR_UE | FPRF(0x04));
  /* tininess before rounding (Book I; unconfirmed on Xenon): exact 2^-1022 * (1 - 2^-104) rounds up to 2^-1022, UX */
  reset(0);
  ab(0x3FEFFFFFFFFFFFFEull, 0x0010000000000001ull);
  run(xts_enc(XI_fmulx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x0010000000000000ull);
  HT_EQ_U64(O.fpscr, 0x8A064000u);
  /* overflow disabled: inf (RN=0) / max (RN=1); FR FI XX OX */
  reset(0);
  ab(0x7FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull);
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x7FF0000000000000ull);
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_OX | XTS_FPSCR_XX | XTS_FPSCR_FR | XTS_FPSCR_FI | FPRF(0x05));
  reset(1);
  ab(0x7FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull);
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x7FEFFFFFFFFFFFFFull);
  /* overflow enabled: exponent - 1536, exact */
  reset(XTS_FPSCR_OE);
  ab(0x7FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull);
  run(xts_enc(XI_faddx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x1FFFFFFFFFFFFFFFull);
  HT_EQ_U64(O.fpscr, FX | FEX | XTS_FPSCR_OX | XTS_FPSCR_OE | FPRF(0x04));
  /* single: frsp of a value above the single range, RN=1 -> largest single */
  reset(1);
  S.fpr[3] = 0x4800000000000000ull;
  run(xts_enc(XI_frspx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0x47EFFFFFE0000000ull);
  /* single denormal result classifies as denormal */
  reset(0);
  S.fpr[3] = 0x36A0000000000000ull;                              /* 2^-149 */
  run(xts_enc(XI_frspx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0x36A0000000000000ull);
  HT_EQ_U64(O.fpscr, FPRF(0x14));
  /* Xenon: fadds with an operand outside the single range -> default QNaN, no exception */
  reset(0);
  ab(0x3FF0000000000000ull, 0x0010000000000000ull);
  run(xts_enc(XI_faddsx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], XM_DNAN64);
  HT_EQ_U64(O.fpscr, FPRF(0x11));
  /* ... but fdivs computes in double */
  run(xts_enc(XI_fdivsx, 0, 1, 2, 3));
  HT_EQ_U64(O.fpr[1], 0x7FD0000000000000ull);
  /* fused single: 1 + 2^-30 * 2^-30 rounds once */
  reset(2);
  S.fpr[2] = 0x3E10000000000000ull;                              /* 2^-30 */
  S.fpr[4] = 0x3E10000000000000ull;
  S.fpr[3] = 0x3FF0000000000000ull;
  run(xts_enc(XI_fmaddsx, 0, 1, 2, 4, 3));
  HT_EQ_U64(O.fpr[1], 0x3FF0000020000000ull);                    /* RN=+inf: 1 + 2^-23 */
}

HTEST(mfp_convert_fpscr)
{
  reset(0);
  S.fpr[3] = 0x4004000000000000ull;                              /* 2.5 -> 2 (ties to even) */
  run(xts_enc(XI_fctiwx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 2);
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_XX | XTS_FPSCR_FI);
  S.fpr[3] = 0xC004000000000000ull;                              /* -2.5 -> -2, sign-extended */
  run(xts_enc(XI_fctiwx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0xFFFFFFFFFFFFFFFEull);
  S.fpr[3] = 0x41E65A0BC0000000ull;                              /* 3e9 -> saturate */
  run(xts_enc(XI_fctiwzx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0x7FFFFFFFull);
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXCVI);
  run(xts_enc(XI_fctidx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 3000000000ull);
  S.fpr[3] = 0x7FF0000000000001ull;
  run(xts_enc(XI_fctiwx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0xFFFFFFFF80000000ull);
  HT_EQ_U64(O.fpscr, FX | VX | XTS_FPSCR_VXCVI | XTS_FPSCR_VXSNAN);
  S.fpr[3] = 0x8000000000000000ull;                              /* -2^63 is representable */
  S.fpr[3] = 0xC3E0000000000000ull;
  run(xts_enc(XI_fctidx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0x8000000000000000ull);
  HT_EQ_U64(O.fpscr, 0);
  S.fpr[3] = 0x7FFFFFFFFFFFFFFFull;                              /* fcfid rounds (RN=0) */
  run(xts_enc(XI_fcfidx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0x43E0000000000000ull);
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_XX | XTS_FPSCR_FR | XTS_FPSCR_FI | FPRF(0x04));
  /* FPSCR instructions */
  reset(3);
  run(xts_enc(XI_mtfsb1x, 0, 29));                               /* NI cannot be set */
  HT_EQ_U64(O.fpscr, 3);
  run(xts_enc(XI_mtfsb1x, 0, 2));                                /* VX cannot be set explicitly */
  HT_EQ_U64(O.fpscr, 3);
  run(xts_enc(XI_mtfsb1x, XTS_RC, 4));                           /* UX: FX too, CR1 */
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_UX | 3);
  HT_EQ_U64(xm_cr_field(&O, 1), 8);
  run(xts_enc(XI_mtfsfix, 0, 0, 15));                            /* FX, OX from U; FEX/VX by rule */
  HT_EQ_U64(O.fpscr, FX | XTS_FPSCR_OX | 3);
  run(xts_enc(XI_mtfsfix, 0, 1, 8));                             /* field 1: no FX */
  HT_EQ_U64(O.fpscr, XTS_FPSCR_UX | 3);
  reset(FX | XTS_FPSCR_OX | XTS_FPSCR_UX | XTS_FPSCR_VXSNAN | 1);
  run(xts_enc(XI_mcrfs, 0, 2, 0));                               /* copy field 0, clear FX OX */
  HT_EQ_U64(xm_cr_field(&O, 2), 0x9 | 0x2);                      /* FX . VX OX: 1 0 1 1 */
  HT_EQ_U64(O.fpscr, VX | XTS_FPSCR_UX | XTS_FPSCR_VXSNAN | 1);
  reset(0x12345671u & ~(FEX | VX | XTS_FPSCR_NI | 0x800u));
  run(xts_enc(XI_mffsx, 0, 5));
  HT_EQ_U64(O.fpr[5] >> 32, 0xFFF80000u);
  HT_EQ_U64((uint32_t)O.fpr[5], (uint32_t)O.fpscr);
}
