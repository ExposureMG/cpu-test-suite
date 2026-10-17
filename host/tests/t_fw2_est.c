/* framework v2: estimate verdict (XTS_M_ESTIMATE path of xts_semantic_check) - bound, exact lanes, unknown lanes,
 * unexpected state changes. */
#include <math.h>
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

static uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

HTEST(fw2_estimate_kind)
{
  int v = -1;
  HT_EQ_U64(xts_estimate_kind(XI_fresx, &v), 0);
  HT_EQ_U64(v, 0);
  HT_EQ_U64(xts_estimate_kind(XI_frsqrtex, &v), 1);
  HT_EQ_U64(xts_estimate_kind(XI_vrefp128, &v), 0);
  HT_EQ_U64(v, 1);
  HT_EQ_U64(xts_estimate_kind(XI_vexptefp, &v), 2);
  HT_EQ_U64(xts_estimate_kind(XI_vlogefp128, &v), 3);
  HT_EQ_U64(xts_estimate_kind(XI_addx, &v), (uint64_t)-1);
}

HTEST(fw2_estimate_scalar)
{
  static xts_state_t in, out;
  char buf[2048];
  xts_json_t j;
  uint32_t w = xts_enc(XI_fresx, 0, 1, 2);
  xts_state_baseline(&in);
  in.fpr[2] = d2u(3.0);
  out = in;
  xm_state_loaded(&out);
  out.fpr[1] = d2u((double)(float)(1.0 / 3.0 * (1 + 1.0 / 512)));     /* within 2^-8 */
  out.fpscr = 0x00004000;                                             /* FPRF: allowed to change */
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  HT_EQ_U64(xts_estimate_verdict(XI_fresx, w, &in, &out, &j), XTS_ST_PASS);
  xj_finish(&j);
  HT_CHECK(strstr(buf, "\"verdict\":\"bound\"") && strstr(buf, "\"ok\":true") && strstr(buf, "\"bound_bits\":8"));
  out.fpr[1] = d2u(1.0 / 3.0 * (1 + 1.0 / 64));                     /* 2^-6 off: outside */
  HT_EQ_U64(xts_estimate_verdict(XI_fresx, w, &in, &out, NULL), XTS_ST_FAIL);
  out.fpr[1] = d2u(1.0 / 3.0);
  out.gpr[7] ^= 1;                                                    /* something else changed */
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  HT_EQ_U64(xts_estimate_verdict(XI_fresx, w, &in, &out, &j), XTS_ST_FAIL);
  xj_finish(&j);
  HT_CHECK(strstr(buf, "\"unexpected_changes\":[\"gpr7\"]") != NULL);
  out.gpr[7] ^= 1;
  /* frsqrte: 2^-5 */
  w = xts_enc(XI_frsqrtex, XTS_RC, 1, 2);
  in.fpr[2] = out.fpr[2] = d2u(4.0);
  out.fpr[1] = d2u(0.5 * (1 + 1.0 / 40));
  xm_set_cr_field(&out, 1, 0x4);                                      /* CR1 from Rc: allowed */
  HT_EQ_U64(xts_estimate_verdict(XI_frsqrtex, w, &in, &out, NULL), XTS_ST_PASS);
  xm_set_cr_field(&out, 2, 0x4);                                      /* CR2: not allowed */
  HT_EQ_U64(xts_estimate_verdict(XI_frsqrtex, w, &in, &out, NULL), XTS_ST_FAIL);
}

HTEST(fw2_estimate_vector)
{
  static xts_state_t in, out;
  char buf[4096];
  xts_json_t j;
  uint32_t w = xts_enc(XI_vrefp, 0, 3, 4);
  xts_state_baseline(&in);
  in.vr[4] = xts_vr4(f2u(2.0f), 0, f2u(-8.0f), f2u(3.0f));            /* lane 1 = +0: exact (+inf) */
  out = in;
  xm_state_loaded(&out);
  out.vr[3] = xts_vr4(f2u(0.5f * (1 + 1.0f / 8192)), 0x7F800000u, f2u(-0.125f), f2u(1.0f / 3.0f));
  xj_init(&j, buf, sizeof buf);
  xj_obj(&j);
  HT_EQ_U64(xts_estimate_verdict(XI_vrefp, w, &in, &out, &j), XTS_ST_PASS);
  xj_finish(&j);
  HT_EQ_U64(ht_count_substr(buf, "\"verdict\":\"bound\""), 3);
  HT_EQ_U64(ht_count_substr(buf, "\"verdict\":\"exact\""), 1);
  HT_CHECK(strstr(buf, "\"exact\":\"7F800000\"") != NULL);
  out.vr[3].w[1] = 0x7F7FFFFFu;                                       /* exact lane wrong */
  HT_EQ_U64(xts_estimate_verdict(XI_vrefp, w, &in, &out, NULL), XTS_ST_FAIL);
  out.vr[3].w[1] = 0x7F800000u;
  out.vr[3].w[3] = f2u(0.3f);                                         /* 10 % off */
  HT_EQ_U64(xts_estimate_verdict(XI_vrefp, w, &in, &out, NULL), XTS_ST_FAIL);
  /* VMX128 form, same lanes */
  w = xts_enc(XI_vrefp128, 0, 100, 70);
  in.vr[70] = in.vr[4];
  out = in;
  xm_state_loaded(&out);
  out.vr[100] = xts_vr4(f2u(0.5f), 0x7F800000u, f2u(-0.125f), f2u(1.0f / 3.0f));
  HT_EQ_U64(xts_estimate_verdict(XI_vrefp128, w, &in, &out, NULL), XTS_ST_PASS);
  /* vexptefp of non-integral inputs: no bound known -> info */
  w = xts_enc(XI_vexptefp, 0, 3, 4);
  in.vr[4] = xts_vr4(f2u(0.5f), f2u(1.5f), f2u(-0.25f), f2u(2.75f));
  out = in;
  xm_state_loaded(&out);
  out.vr[3] = xts_vr4(f2u(1.41f), f2u(2.83f), f2u(0.84f), f2u(6.7f));
  HT_EQ_U64(xts_estimate_verdict(XI_vexptefp, w, &in, &out, NULL), XTS_ST_INFO);
  /* ... with one integral lane the model knows exactly: 2^2 = 4 */
  in.vr[4].w[0] = out.vr[4].w[0] = f2u(2.0f);
  out.vr[3].w[0] = f2u(4.0f);
  HT_EQ_U64(xts_estimate_verdict(XI_vexptefp, w, &in, &out, NULL), XTS_ST_PASS);
  out.vr[3].w[0] = f2u(4.5f);
  HT_EQ_U64(xts_estimate_verdict(XI_vexptefp, w, &in, &out, NULL), XTS_ST_FAIL);
  HT_EQ_U64(xts_estimate_verdict(XI_addx, xts_enc(XI_addx, 0, 1, 2, 3), &in, &out, NULL), XTS_ST_FAIL);
}
