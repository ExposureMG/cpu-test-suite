/* src/model/vmx128.c: hardware known answers (results4 / fmastat words and values) for the VMX128-only handlers. */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

static xts_state_t in, out;

static xts_mres_t run(uint32_t w)
{
  xts_mem_t m;
  xts_mem_init(&m);
  return xts_model_exec(NULL, w, &in, &m, &out);
}

static void setv(int r, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { in.vr[r] = xts_vr4(a, b, c, d); }
static int isv(int r, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
  return out.vr[r].w[0] == a && out.vr[r].w[1] == b && out.vr[r].w[2] == c && out.vr[r].w[3] == d;
}
static int splat(int r, uint32_t v) { return isv(r, v, v, v, v); }

HTEST(mvmx_vmsum_quirks)
{
  xts_state_baseline(&in);
  /* vmsum4fp128 v26,v19,v6: -1.5 + -0.75 with a +0 product -> one LSB low (results4) */
  in.vscr.w[3] = XTS_VSCR_NJ;
  setv(19, 0x00000000, 0x00000000, 0x3FC00000, 0x3FC00000);
  setv(6, 0x80000000, 0x3F800000, 0xBF800000, 0xBF000000);
  HT_EQ_U64(run(0x175331D0), XTS_M_OK);
  HT_CHECK(splat(26, 0xC00FFFFF));
  /* the same word built by the encoder */
  HT_EQ_U64(xts_enc(XI_vmsum4fp128, 0, 26, 19, 6), 0x175331D0);
  /* vmsum4fp128 v4,v1,v18: big negative minus small positives stays exact */
  setv(1, 0x477FE000, 0x4B000000, 0x3F800000, 0x7F7FC99E);
  setv(18, 0x4B000000, 0x00000000, 0x3F800000, 0xBF000000);
  HT_EQ_U64(run(0x148191D0), XTS_M_OK);
  HT_CHECK(splat(4, 0xFEFFC99E));
  /* vmsum3fp128 v7,v13,v15: Y + 2.5 + (-0) -> Y - 1 ulp; lane w (NaN) ignored */
  in.vscr.w[3] = XTS_VSCR_SAT;
  setv(13, 0x7F7FC99E, 0xBF800000, 0x00800000, 0x7FC00000);
  setv(15, 0x3F800000, 0xC0200000, 0x80000000, 0xC0200000);
  HT_EQ_U64(run(0x14ED7990), XTS_M_OK);
  HT_CHECK(splat(7, 0x7F7FC99D));
  /* overflow -> signed QNaN */
  in.vscr.w[3] = 0;
  setv(27, 0x3FC00000, 0xC0200000, 0xC0200000, 0x00800000);
  setv(20, 0x00000001, 0x007FFFFF, 0x7F7FC99E, 0x4B000000);
  HT_EQ_U64(run(0x149BA190), XTS_M_OK);
  HT_CHECK(splat(4, 0xFFC00000));
  /* denormal result flushed to -0 although NJ=0 */
  in.vscr.w[3] = XTS_VSCR_SAT;
  setv(4, 0x00000000, 0x007FFFFF, 0x007FFFFF, 0x7F800000);
  setv(19, 0x00000000, 0x007FFFFF, 0xBF000000, 0x477FE000);
  HT_EQ_U64(run(0x14049990), XTS_M_OK);
  HT_CHECK(splat(0, 0x80000000));
  HT_EQ_U64(out.vscr.w[3], XTS_VSCR_SAT);          /* VSCR untouched */
}

HTEST(mvmx_fma128)
{
  xts_state_baseline(&in);
  /* fmastat vmaddfp128:0 (VD=VS=v18) */
  setv(99, 0xbf5ef2e0, 0xc003b671, 0x3e8e7d88, 0x401fea11);
  setv(40, 0x4158608f, 0xbfe3529c, 0x3fef2938, 0x409bbdcd);
  setv(18, 0xc15eb4ff, 0xbed31199, 0x3fc599f4, 0x3f174e41);
  HT_EQ_U64(run(0x164344F1), XTS_M_OK);
  HT_CHECK(isv(18, 0xC1CD9302, 0x404F885B, 0x40041483, 0x414C0774));
  /* vnmsubfp128: exact cancellation gives -0; NaN quieted, not negated; NJ flushes a denormal sum */
  uint32_t w = xts_enc(XI_vnmsubfp128, 0, 5, 6, 7, 5);
  setv(6, 0x3F800000, 0x7F800001, 0x00800000, 0x7F800000);
  setv(7, 0x40000000, 0x3F800000, 0x3F000000, 0x00000000);
  setv(5, 0x40000000, 0x3F800000, 0x80000000, 0x3F800000);
  in.vscr.w[3] = XTS_VSCR_NJ;
  HT_EQ_U64(run(w), XTS_M_OK);
  HT_CHECK(isv(5, 0x80000000, 0x7FC00001, 0x80000000, 0x7FC00000));
}

HTEST(mvmx_perm_d3d)
{
  xts_state_baseline(&in);
  /* vrlimi128 v0,v2,5,? (results4) */
  setv(2, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF);
  setv(0, 0xD29318D1, 0xE2DE8515, 0x7DDBADA5, 0x2F33E845);
  HT_EQ_U64(run(0x18051750), XTS_M_OK);
  HT_CHECK(isv(0, 0xD29318D1, 0x7FFFFFFF, 0x7DDBADA5, 0x7FFFFFFF));
  /* vpermwi128 reverse (PERM 0xE4) */
  setv(9, 1, 2, 3, 4);
  HT_EQ_U64(run(xts_enc(XI_vpermwi128, 0, 8, 9, 0xE4)), XTS_M_OK);
  HT_CHECK(isv(8, 4, 3, 2, 1));
  /* vpkd3d128 2_10_10_10, pack 1, z 0: SNaN -> -512, sets SAT (results4) */
  in.vscr.w[3] = XTS_VSCR_NJ;
  setv(21, 0x7F800001, 0xFF800000, 0x7F7FC99E, 0x3FC00000);
  setv(14, 0x7F800001, 0xFF800000, 0x7FC00000, 0x007FFFFF);
  HT_EQ_U64(run(0x19C9AE10), XTS_M_OK);
  HT_CHECK(isv(14, 0x7F800001, 0xFF800000, 0x7FC00000, 0x1FF80600));
  HT_EQ_U64(out.vscr.w[3], XTS_VSCR_NJ | XTS_VSCR_SAT);
  /* vupkd3d128 NORMSHORT2 (UIMM 5, results4) */
  setv(20, 0x00000001, 0x4B800000, 0x7F800001, 0xFF800000);
  HT_EQ_U64(run(0x19A5A7F0), XTS_M_OK);
  HT_CHECK(isv(13, 0x403FFF80, 0x40400000, 0x00000000, 0x3F800000));
  /* pack -> unpack round trip of an in-window NORMSHORT4 vector, NaN lane -> 0x8000 -> QNaN */
  setv(30, 0x40400000 + 5, 0x40400000 - 7, 0x7FC00000, 0x40400000 + 32767);
  setv(31, 0, 0, 0, 0);
  in.vscr.w[3] = 0;
  HT_EQ_U64(run(xts_enc(XI_vpkd3d128, 0, 31, 30, 4, 2, 0)), XTS_M_OK);
  HT_CHECK(isv(31, 0, 0, 0x0005FFF9, 0x80007FFF));
  HT_EQ_U64(out.vscr.w[3], 0);
  in.vr[31] = out.vr[31];
  HT_EQ_U64(run(xts_enc(XI_vupkd3d128, 0, 32, 31, 16)), XTS_M_OK);
  HT_CHECK(isv(32, 0x40400005, 0x403FFFF9, 0x7FC00000, 0x40407FFF));
  /* not modelled: pack size 0 */
  HT_EQ_U64(run(xts_enc(XI_vpkd3d128, 0, 31, 30, 4, 0, 0)), XTS_M_UNIMPLEMENTED);
}
