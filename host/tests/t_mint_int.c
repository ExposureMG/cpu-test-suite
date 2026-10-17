/* mint: hand-computed vectors for the integer / rotate / CR / trap model handlers */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

static xts_mres_t run(uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  xts_mem_t m;
  xts_mem_init(&m);
  return xts_model_exec(NULL, w, in, &m, out);
}
static void base(xts_state_t *s, int sf)
{
  xts_state_baseline(s);
  s->mode = sf ? XTS_MODE_SF : 0;
  s->cr = 0;
  s->xer = 0;
}
static uint32_t crf(const xts_state_t *s, int f) { return ((uint32_t)s->cr >> (28 - 4 * f)) & 0xF; }

HTEST(mint_arith)
{
  xts_state_t in, out;
  /* addic: carry out of bit 32 in 32-bit mode only */
  base(&in, 0);
  in.gpr[4] = 0xFFFFFFFFull;
  HT_EQ_U64(run(xts_enc(XI_addic, 0, 3, 4, 1), &in, &out), XTS_M_OK);
  HT_EQ_U64(out.gpr[3], 0x100000000ull);
  HT_EQ_U64(out.xer & XTS_XER_CA, XTS_XER_CA);
  in.mode = XTS_MODE_SF;
  run(xts_enc(XI_addic, 0, 3, 4, 1), &in, &out);
  HT_EQ_U64(out.xer & XTS_XER_CA, 0);
  /* subfc: 0 - 1 borrows, 0 - 0 carries */
  base(&in, 1);
  in.gpr[4] = 1; in.gpr[5] = 0;
  run(xts_enc(XI_subfcx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], ~0ull);
  HT_EQ_U64(out.xer & XTS_XER_CA, 0);
  in.gpr[4] = 0;
  run(xts_enc(XI_subfcx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0);
  HT_EQ_U64(out.xer & XTS_XER_CA, XTS_XER_CA);
  /* adde with carry in */
  in.xer = XTS_XER_CA;
  in.gpr[4] = ~0ull; in.gpr[5] = 0;
  run(xts_enc(XI_addex, XTS_OE | XTS_RC, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0);
  HT_EQ_U64(out.xer, XTS_XER_CA);
  HT_EQ_U64(crf(&out, 0), 2);
  /* neg overflow depends on the mode */
  base(&in, 0);
  in.gpr[4] = 0x80000000ull;
  run(xts_enc(XI_negx, XTS_OE, 3, 4), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFF80000000ull);
  HT_EQ_U64(out.xer, XTS_XER_OV | XTS_XER_SO);
  in.mode = XTS_MODE_SF;
  run(xts_enc(XI_negx, XTS_OE, 3, 4), &in, &out);
  HT_EQ_U64(out.xer, 0);
  /* mullw overflow, mulhw sign extension, mulhwu */
  base(&in, 1);
  in.gpr[4] = 0x10000; in.gpr[5] = 0x10000;
  run(xts_enc(XI_mullwx, XTS_OE | XTS_RC, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x100000000ull);
  HT_EQ_U64(out.xer, XTS_XER_OV | XTS_XER_SO);
  HT_EQ_U64(crf(&out, 0), 4 | 1);
  in.gpr[4] = (uint64_t)-2; in.gpr[5] = 0x7FFFFFFF;
  run(xts_enc(XI_mulhwx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], ~0ull);
  in.gpr[4] = 0xFFFFFFFF; in.gpr[5] = 0xFFFFFFFF;
  run(xts_enc(XI_mulhwux, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFEull);
  in.gpr[4] = 0xFFFFFFFFFFFFFFFFull; in.gpr[5] = 0xFFFFFFFFFFFFFFFFull;
  run(xts_enc(XI_mulhdux, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFFFFFFFFFEull);
  run(xts_enc(XI_mulhdx, 0, 3, 4, 5), &in, &out);           /* (-1)*(-1) = 1 */
  HT_EQ_U64(out.gpr[3], 0);
  run(xts_enc(XI_mulldx, XTS_OE, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 1);
  HT_EQ_U64(out.xer, 0);
  in.gpr[4] = 0x4000000000000000ull; in.gpr[5] = 4;
  run(xts_enc(XI_mulldx, XTS_OE, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0);
  HT_EQ_U64(out.xer, XTS_XER_OV | XTS_XER_SO);
  /* mulh with OE and Rc is a reserved form: illegal */
  HT_EQ_U64(run(0x7C642E97, &in, &out), XTS_M_ILLEGAL);     /* mulhw. with OE */
  /* divide */
  in.gpr[4] = (uint64_t)-7; in.gpr[5] = 2;
  run(xts_enc(XI_divwx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFDull);
  run(xts_enc(XI_divdx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], (uint64_t)-3);
  run(xts_enc(XI_divwux, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x7FFFFFFCull);                     /* 0xFFFFFFF9 / 2 */
  in.gpr[5] = 0;
  run(xts_enc(XI_divwx, XTS_OE, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.xer, XTS_XER_OV | XTS_XER_SO);
}

HTEST(mint_logic_rot)
{
  xts_state_t in, out;
  base(&in, 1);
  in.gpr[4] = 0x12345678ull;
  run(xts_enc(XI_rlwinmx, 0, 3, 4, 8, 24, 31), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x12);
  run(xts_enc(XI_rlwinmx, 0, 3, 4, 4, 28, 3), &in, &out);   /* wrapped mask 0xF000000F */
  HT_EQ_U64(out.gpr[3], 0x2345678120000001ull);              /* MASK(60,35) wraps into the high word */
  in.gpr[4] = 0x8000000000000001ull;
  run(xts_enc(XI_rldiclx, 0, 3, 4, 1, 60), &in, &out);
  HT_EQ_U64(out.gpr[3], 3);
  run(xts_enc(XI_rldicrx, 0, 3, 4, 1, 1), &in, &out);       /* rotl 1 = 3, keep bits 0..1 */
  HT_EQ_U64(out.gpr[3], 0);
  run(xts_enc(XI_rldicx, 0, 3, 4, 4, 0), &in, &out);        /* rotl 4 = 0x18, mask(0, 59) */
  HT_EQ_U64(out.gpr[3], 0x10);
  in.gpr[3] = ~0ull; in.gpr[4] = 0xAB;
  run(xts_enc(XI_rldimix, 0, 3, 4, 8, 48), &in, &out);      /* insert 0xAB00 into bits 48..55 */
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFFFFFFABFFull);
  run(xts_enc(XI_rlwimix, 0, 3, 4, 8, 16, 23), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFFFFFFABFFull);
  /* shifts */
  in.gpr[4] = 1; in.gpr[5] = 32;
  run(xts_enc(XI_slwx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0);
  in.gpr[5] = 31;
  run(xts_enc(XI_slwx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x80000000ull);
  in.gpr[5] = 64;
  run(xts_enc(XI_sldx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], 0);
  in.gpr[4] = (uint64_t)-7; in.gpr[5] = 1;
  run(xts_enc(XI_srawx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], (uint64_t)-4);
  HT_EQ_U64(out.xer & XTS_XER_CA, XTS_XER_CA);
  in.gpr[4] = (uint64_t)-8;
  run(xts_enc(XI_srawx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.xer & XTS_XER_CA, 0);
  in.gpr[5] = 40;
  run(xts_enc(XI_srawx, 0, 3, 4, 5), &in, &out);
  HT_EQ_U64(out.gpr[3], ~0ull);
  HT_EQ_U64(out.xer & XTS_XER_CA, XTS_XER_CA);
  in.gpr[4] = 0x80000000ull;                                /* positive 64-bit but negative word */
  run(xts_enc(XI_srawix, XTS_RC, 3, 4, 4), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFFF8000000ull);
  HT_EQ_U64(crf(&out, 0), 8);
  in.gpr[4] = (uint64_t)-5;
  run(xts_enc(XI_sradix, 0, 3, 4, 1), &in, &out);
  HT_EQ_U64(out.gpr[3], (uint64_t)-3);
  HT_EQ_U64(out.xer & XTS_XER_CA, XTS_XER_CA);
  /* counts, extends, logical immediates */
  in.gpr[4] = 0;
  run(xts_enc(XI_cntlzwx, 0, 3, 4), &in, &out);
  HT_EQ_U64(out.gpr[3], 32);
  run(xts_enc(XI_cntlzdx, 0, 3, 4), &in, &out);
  HT_EQ_U64(out.gpr[3], 64);
  in.gpr[4] = 0x80;
  run(xts_enc(XI_extsbx, XTS_RC, 3, 4), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xFFFFFFFFFFFFFF80ull);
  HT_EQ_U64(crf(&out, 0), 8);
  in.gpr[4] = 0xF0F0;
  run(xts_enc(XI_andi_, 0, 3, 4, 0x00FF), &in, &out);
  HT_EQ_U64(out.gpr[3], 0xF0);
  HT_EQ_U64(crf(&out, 0), 4);
  run(xts_enc(XI_xoris, 0, 3, 4, 0x8000), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x8000F0F0ull);
  /* compare width */
  base(&in, 1);
  in.gpr[4] = 0x100000000ull;
  run(xts_enc(XI_cmpi, 0, 2, 0, 4, 0), &in, &out);
  HT_EQ_U64(crf(&out, 2), 2);                               /* L=0: low word 0 == 0 */
  run(xts_enc(XI_cmpi, 0, 2, 1, 4, 0), &in, &out);
  HT_EQ_U64(crf(&out, 2), 4);                               /* L=1: GT */
}

HTEST(mint_cr_spr_trap)
{
  xts_state_t in, out;
  base(&in, 1);
  in.cr = 0x12345678;
  in.gpr[4] = 0xFFFFFFFFull;
  run(xts_enc(XI_mfcr, 0, 3), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x12345678);
  /* mtocrf: only the most significant selected field is written; mtcrf: all of them */
  run(0x7C100120u | (4u << 21) | (0x30u << 12), &in, &out);   /* mtocrf 0x30, r4 */
  HT_EQ_U64(out.cr, 0x12F45678ull);                         /* field 2 := r4 field 2 (0xF); field 3 untouched */
  run(xts_enc(XI_mtcrf, 0, 0x30, 4), &in, &out);
  HT_EQ_U64(out.cr, 0x12FF5678ull);
  run(xts_enc(XI_crxor, 0, 3, 0, 0), &in, &out);             /* bit3 := bit0 ^ bit0 = 0 */
  HT_EQ_U64(out.cr, 0x02345678ull);
  run(xts_enc(XI_crorc, 0, 31, 3, 0), &in, &out);            /* bit31 := bit3 | ~bit0 = 1 | 1 */
  HT_EQ_U64(out.cr & 1, 1);
  run(xts_enc(XI_mcrf, 0, 7, 0), &in, &out);
  HT_EQ_U64(out.cr, 0x12345671ull);
  /* SPR moves */
  in.lr = 0x1111; in.ctr = 0x2222; in.xer = XTS_XER_CA;
  run(xts_enc(XI_mfspr, 0, 3, 8), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x1111);
  run(xts_enc(XI_mfspr, 0, 3, 9), &in, &out);
  HT_EQ_U64(out.gpr[3], 0x2222);
  run(xts_enc(XI_mfspr, 0, 3, 1), &in, &out);
  HT_EQ_U64(out.gpr[3], XTS_XER_CA);
  in.gpr[4] = 0x123456789ull;
  run(xts_enc(XI_mtspr, 0, 9, 4), &in, &out);
  HT_EQ_U64(out.ctr, 0x123456789ull);
  HT_EQ_U64(run(xts_enc(XI_mfspr, 0, 3, 268), &in, &out), XTS_M_UNIMPLEMENTED);
  /* traps */
  in.gpr[4] = 5; in.gpr[5] = 5;
  HT_EQ_U64(run(xts_enc(XI_tw, 0, 4, 4, 5), &in, &out), XTS_M_TRAP);       /* TO=4: equal */
  HT_EQ_U64(run(xts_enc(XI_tw, 0, 3, 4, 5), &in, &out), XTS_M_OK);         /* TO=3: ult/ugt, equal -> no trap */
  in.gpr[4] = 0x180000000ull; in.gpr[5] = 1;                                /* low word negative */
  HT_EQ_U64(run(xts_enc(XI_tw, 0, 16, 4, 5), &in, &out), XTS_M_TRAP);      /* signed < on words */
  HT_EQ_U64(run(xts_enc(XI_td, 0, 16, 4, 5), &in, &out), XTS_M_OK);        /* 64-bit: positive, not < */
  HT_EQ_U64(run(xts_enc(XI_td, 0, 1, 4, 5), &in, &out), XTS_M_TRAP);       /* unsigned > */
  HT_EQ_U64(run(xts_enc(XI_twi, 0, 8, 5, -1), &in, &out), XTS_M_TRAP);     /* r5=1 signed > -1 */
  HT_EQ_U64(run(xts_enc(XI_tdi, 0, 2, 5, -1), &in, &out), XTS_M_TRAP);     /* 1 unsigned < 0xFFFF... */
  HT_EQ_U64(run(xts_enc(XI_tdi, 0, 1, 5, -1), &in, &out), XTS_M_OK);
}
