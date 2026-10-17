/* mfp: vector FP model - NJ flush / Java mode, NaN order, saturation, compares, estimate specials */
#include <string.h>
#include <xts/xts.h>
#include <model/model_priv.h>   /* xf_estimate_ok */
#include "htest.h"

static xts_state_t S, O;

static xts_mres_t run(uint32_t w)
{
  xts_mem_t m;
  xts_mem_init(&m);
  return xts_model_exec(NULL, w, &S, &m, &O);
}
static void reset(int nj)
{
  xts_state_baseline(&S);
  S.vscr.w[3] = nj ? XTS_VSCR_NJ : 0;
}
#define EQV(v, a, b, c, d)            \
  do {                                \
    HT_EQ_U64((v).w[0], a);           \
    HT_EQ_U64((v).w[1], b);           \
    HT_EQ_U64((v).w[2], c);           \
    HT_EQ_U64((v).w[3], d);           \
  } while (0)

HTEST(mfp_vec_arith)
{
  /* denormal + denormal: gradual with NJ=0, flushed with NJ=1 (inputs and results keep their sign) */
  reset(0);
  S.vr[2] = xts_vr4(0x00000001, 0x80400000, 0x3F800000, 0x7F800000);
  S.vr[3] = xts_vr4(0x00000001, 0x80400000, 0x00000001, 0xFF800000);
  HT_EQ_U64(run(xts_enc(XI_vaddfp, 0, 1, 2, 3)), XTS_M_OK);
  EQV(O.vr[1], 0x00000002, 0x80800000, 0x3F800000, 0x7FC00000);
  reset(1);
  S.vr[2] = xts_vr4(0x00000001, 0x80400000, 0x3F800000, 0x00400000);
  S.vr[3] = xts_vr4(0x00000001, 0x80400000, 0x00000001, 0x80000000);
  run(xts_enc(XI_vaddfp, 0, 1, 2, 3));
  EQV(O.vr[1], 0x00000000, 0x80000000, 0x3F800000, 0x00000000);
  /* NJ=1 flushes a tiny product (2^-64 * 2^-64 = 2^-128), NJ=0 keeps the denormal */
  S.vr[2] = xts_vr4(0x1F800000, 0x9F800000, 0, 0);
  S.vr[3] = xts_vr4(0x1F800000, 0x1F800000, 0, 0);
  run(xts_enc(XI_vmulfp128, 0, 1, 2, 3));
  EQV(O.vr[1], 0, 0x80000000, 0, 0);
  S.vscr.w[3] = 0;
  run(xts_enc(XI_vmulfp128, 0, 1, 2, 3));
  EQV(O.vr[1], 0x00200000, 0x80200000, 0, 0);
  /* NaN order: vA, vB, vC; payload kept, quiet bit set */
  reset(0);
  S.vr[2] = xts_vr4(0x3F800000, 0x7F800001, 0x3F800000, 0x7F800000);
  S.vr[3] = xts_vr4(0x7F800002, 0x7FC00003, 0x3F800000, 0x7F800000);   /* vC */
  S.vr[4] = xts_vr4(0x7FC00004, 0x3F800000, 0x7F800005, 0xFF800000);   /* vB */
  run(xts_enc(XI_vmaddfp, 0, 1, 2, 3, 4));
  EQV(O.vr[1], 0x7FC00004, 0x7FC00001, 0x7FC00005, 0x7FC00000);
  /* vnmsubfp negates after rounding: exact cancellation gives -0 */
  S.vr[2] = xts_vr4(0x3F800000, 0, 0, 0);
  S.vr[3] = xts_vr4(0x3F800000, 0, 0, 0);
  S.vr[4] = xts_vr4(0x3F800000, 0, 0, 0);
  run(xts_enc(XI_vnmsubfp, 0, 1, 2, 3, 4));
  HT_EQ_U64(O.vr[1].w[0], 0x80000000);
  /* vector ops never touch FPSCR */
  HT_EQ_U64(O.fpscr, S.fpscr);
  /* max/min with zeros and NaN */
  S.vr[2] = xts_vr4(0x00000000, 0x80000000, 0x7FC00001, 0x3F800000);
  S.vr[3] = xts_vr4(0x80000000, 0x00000000, 0x3F800000, 0x7F800002);
  run(xts_enc(XI_vmaxfp, 0, 1, 2, 3));
  EQV(O.vr[1], 0x00000000, 0x00000000, 0x7FC00001, 0x7FC00002);
  run(xts_enc(XI_vminfp128, 0, 1, 2, 3));
  EQV(O.vr[1], 0x80000000, 0x80000000, 0x7FC00001, 0x7FC00002);
  /* round to integral */
  S.vr[3] = xts_vr4(0x3FC00000, 0x40200000, 0xBF000000, 0x00000001);   /* 1.5 2.5 -0.5 denormal */
  run(xts_enc(XI_vrfin, 0, 1, 3));
  EQV(O.vr[1], 0x40000000, 0x40000000, 0x80000000, 0x00000000);
  run(xts_enc(XI_vrfip, 0, 1, 3));
  EQV(O.vr[1], 0x40000000, 0x40400000, 0x80000000, 0x3F800000);
  run(xts_enc(XI_vrfim128, 0, 1, 3));
  EQV(O.vr[1], 0x3F800000, 0x40000000, 0xBF800000, 0x00000000);
}

HTEST(mfp_vec_convert_compare)
{
  reset(0);
  S.vr[3] = xts_vr4(0x4F000000, 0xCF800000, 0x7FC00000, 0xBF000000);  /* 2^31, -2^32, NaN, -0.5 */
  run(xts_enc(XI_vctsxs, 0, 1, 3, 0));
  EQV(O.vr[1], 0x7FFFFFFF, 0x80000000, 0, 0);
  HT_EQ_U64(O.vscr.w[3], XTS_VSCR_SAT);
  S.vr[3] = xts_vr4(0x7FC00000, 0xBD000000, 0x3F800000, 0x3F800000);  /* no saturation: SAT stays 0 */
  run(xts_enc(XI_vctuxs, 0, 1, 3, 4));
  EQV(O.vr[1], 0, 0, 16, 16);
  HT_EQ_U64(O.vscr.w[3], 0);
  S.vr[3] = xts_vr4(0xBF800000, 0, 0, 0);                         /* -1 -> 0, saturated */
  run(xts_enc(XI_vcfpuxws128, 0, 1, 3, 0));
  HT_EQ_U64(O.vr[1].w[0], 0);
  HT_EQ_U64(O.vscr.w[3], XTS_VSCR_SAT);
  S.vr[3] = xts_vr4(1, 0xFFFFFFFF, 0x7FFFFFFF, 0);
  run(xts_enc(XI_vcfsx, 0, 1, 3, 1));
  EQV(O.vr[1], 0x3F000000, 0xBF000000, 0x4E800000, 0);           /* 0.5, -0.5, 2^30 (rounded), 0 */
  run(xts_enc(XI_vcuxwfp128, 0, 1, 3, 31));
  EQV(O.vr[1], 0x30000000, 0x40000000, 0x3F800000, 0);
  /* compares; Rc -> CR6 */
  S.vr[2] = xts_vr4(0x3F800000, 0x7FC00000, 0x00000000, 0xC0000000);
  S.vr[3] = xts_vr4(0x3F800000, 0x3F800000, 0x80000000, 0x3F800000);
  run(xts_enc(XI_vcmpeqfp, XTS_RC, 1, 2, 3));
  EQV(O.vr[1], 0xFFFFFFFF, 0, 0xFFFFFFFF, 0);
  HT_EQ_U64(xm_cr_field(&O, 6), 0);
  run(xts_enc(XI_vcmpbfp128, XTS_RC, 1, 2, 3));
  EQV(O.vr[1], 0, 0xC0000000, 0, 0x40000000);
  HT_EQ_U64(xm_cr_field(&O, 6), 0);
  S.vr[2] = xts_vr4(0, 0x3F000000, 0xBF000000, 0x3F800000);
  S.vr[3] = xts_vr4(0x40000000, 0x40000000, 0x40000000, 0x40000000);
  run(xts_enc(XI_vcmpbfp, XTS_RC, 1, 2, 3));
  HT_EQ_U64(xm_cr_field(&O, 6), 2);                               /* all in bounds */
  run(xts_enc(XI_vcmpgtfp128, XTS_RC, 1, 3, 2));
  HT_EQ_U64(xm_cr_field(&O, 6), 8);                               /* all true */
}

HTEST(mfp_vec_estimates)
{
  reset(0);
  S.vr[3] = xts_vr4(0x80000000, 0x7F800000, 0x7F800001, 0x00000001);
  HT_EQ_U64(run(xts_enc(XI_vrefp, 0, 1, 3)), XTS_M_OK);
  EQV(O.vr[1], 0xFF800000, 0x00000000, 0x7FC00001, 0x7F800000);
  S.vr[3] = xts_vr4(0x3F800000, 0, 0, 0);                         /* needs the table */
  HT_EQ_U64(run(xts_enc(XI_vrefp128, 0, 1, 3)), XTS_M_UNIMPLEMENTED);
  S.vr[3] = xts_vr4(0x80000000, 0xBF800000, 0x7F800000, 0x00000000);
  run(xts_enc(XI_vrsqrtefp, 0, 1, 3));
  EQV(O.vr[1], 0xFF800000, 0x7FC00000, 0x00000000, 0x7F800000);
  S.vr[3] = xts_vr4(0x40400000, 0xC0400000, 0xC3160000, 0xFF800000);   /* 3, -3, -150, -inf */
  run(xts_enc(XI_vexptefp, 0, 1, 3));
  EQV(O.vr[1], 0x41000000, 0x3E000000, 0x00000000, 0x00000000);
  S.vr[3] = xts_vr4(0xC3140000, 0x43000000, 0x80000000, 0x00000001);   /* -148, 128, -0, denormal */
  run(xts_enc(XI_vexptefp128, 0, 1, 3));
  EQV(O.vr[1], 0x00000002, 0x7F800000, 0x3F800000, 0x3F800000);
  reset(1);
  S.vr[3] = xts_vr4(0xC3140000, 0x43000000, 0x80000000, 0x00000001);
  run(xts_enc(XI_vexptefp128, 0, 1, 3));
  EQV(O.vr[1], 0x00000000, 0x7F800000, 0x3F800000, 0x3F800000);
  S.vr[3] = xts_vr4(0x41000000, 0x00000004, 0xBF800000, 0x7F800000);   /* 8, denormal (NJ=1 -> 0), -1, inf */
  run(xts_enc(XI_vlogefp, 0, 1, 3));
  EQV(O.vr[1], 0x40400000, 0xFF800000, 0x7FC00000, 0x7F800000);
  S.vscr.w[3] = 0;
  run(xts_enc(XI_vlogefp128, 0, 1, 3));
  HT_EQ_U64(O.vr[1].w[1], 0xC3130000);                            /* log2(2^-147) = -147 */
  /* scalar estimates: specials only */
  xts_state_baseline(&S);
  S.fpscr = 0;
  S.fpr[3] = 0x8000000000000000ull;
  run(xts_enc(XI_fresx, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], 0xFFF0000000000000ull);
  HT_EQ_U64(O.fpscr, XTS_FPSCR_FX | XTS_FPSCR_ZX | (0x09u << 12));
  S.fpr[3] = 0xBFF0000000000000ull;
  run(xts_enc(XI_frsqrtex, 0, 1, 3));
  HT_EQ_U64(O.fpr[1], XM_DNAN64);
  HT_EQ_U64(run(xts_enc(XI_fresx, 0, 1, 3)), XTS_M_UNIMPLEMENTED);
  /* the accuracy check offered for a tolerance channel */
  HT_EQ_U64(xf_estimate_ok(0, 0, 0x4004000000000000ull, 0x3FD9800000000000ull), 1);  /* fres(2.5): error 2^-8 */
  HT_EQ_U64(xf_estimate_ok(0, 1, 0x3FF0000000000000ull, 0x3FEFFC0000000000ull), 0);  /* 2^-11 > vrefp bound */
  HT_EQ_U64(xf_estimate_ok(1, 0, 0x4340000000000000ull, 0x3E46000000000000ull), 1);  /* frsqrte(2^53) */
}
