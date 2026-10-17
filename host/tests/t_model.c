/* reference model framework + the add/or examples */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

static xts_mres_t run(uint32_t w, xts_state_t *in, xts_state_t *out)
{
  xts_mem_t m;
  xts_mem_init(&m);
  return xts_model_exec(NULL, w, in, &m, out);
}

HTEST(model_add_or)
{
  xts_state_t in, out;
  xts_state_baseline(&in);
  in.gpr[4] = 0x7FFFFFFFFFFFFFFFull;
  in.gpr[5] = 1;
  in.xer = 0;
  HT_EQ_U64(run(xts_enc(XI_addx, XTS_OE | XTS_RC, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_EQ_U64(out.gpr[3], 0x8000000000000000ull);
  HT_EQ_U64(out.xer, XTS_XER_OV | XTS_XER_SO);
  HT_EQ_U64((uint32_t)out.cr >> 28, 0x9);                 /* LT + SO */
  /* 32-bit mode: CR0/OV from the low word */
  in.mode = 0;
  in.gpr[4] = 0x000000017FFFFFFFull;
  HT_EQ_U64(run(xts_enc(XI_addx, XTS_OE | XTS_RC, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_EQ_U64(out.gpr[3], 0x0000000180000000ull);
  HT_EQ_U64(out.xer & (XTS_XER_OV | XTS_XER_SO), XTS_XER_OV | XTS_XER_SO);
  HT_EQ_U64((uint32_t)out.cr >> 28, 0x9);
  in.mode = XTS_MODE_SF;
  in.gpr[4] = 0;
  in.gpr[5] = 0;
  in.xer = XTS_XER_SO;
  HT_EQ_U64(run(xts_enc(XI_orx, XTS_RC, 6, 4, 5), &in, &out), XTS_M_OK);
  HT_EQ_U64(out.gpr[6], 0);
  HT_EQ_U64((uint32_t)out.cr >> 28, 0x3);                 /* EQ + SO */
  HT_EQ_U64(out.gpr[7], in.gpr[7]);                       /* nothing else changes */
  /* expected-illegal, unknown word, unimplemented */
  HT_EQ_U64(run(xts_enc(XI_vmulouh, 0, 1, 2, 3), &in, &out), XTS_M_ILLEGAL);
  HT_EQ_U64(run(0, &in, &out), XTS_M_ILLEGAL);
  HT_EQ_U64(run(xts_enc(XI_mftb, 0, 1, 268), &in, &out), XTS_M_UNIMPLEMENTED);   /* never modelled */
  HT_EQ_U64(xts_model_check(NULL), 0);
  HT_CHECK(xts_model_handler_count() >= 2);              /* model families add more */
  /* loaded-state normalisation: FPSCR[NI] cannot be set on Xenon */
  in.fpscr = 4;
  HT_EQ_U64(run(xts_enc(XI_vmulouh, 0, 1, 2, 3), &in, &out), XTS_M_ILLEGAL);
  HT_EQ_U64(out.fpscr, 0);
  HT_EQ_U64(xm_fpscr_merge(0, XTS_FPSCR_VXSNAN), XTS_FPSCR_FX | XTS_FPSCR_VX | XTS_FPSCR_VXSNAN);
  HT_EQ_U64(xm_fprf_f64(0x8000000000000000ull) >> 12, 0x12);
}
