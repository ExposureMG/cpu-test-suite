/* exec engine (host backend), case setup, semantic oracle plumbing */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

HTEST(exec_host)
{
  static xts_exec_t x;
  xts_env_reset();
  xts_env.fpscr_set = 1;
  xts_env.fpscr_clr = 3;
  xts_env.sf = 0;
  xts_case_init(&x);
  HT_EQ_U64(x.in.mode & XTS_MODE_SF, 0);
  HT_EQ_U64(x.in.fpscr, 1);
  HT_EQ_U64(x.in.gpr[5], 0xC0DE00000000055Aull);
  HT_EQ_U64(x.mem_in[3], (uint8_t)(3 * 13 + 7));
  xts_env_reset();
  xts_case_init(&x);
  xts_set_gpr(&x, 4, 40);
  xts_set_gpr(&x, 5, 2);
  HT_EQ_U64(xts_exec_word(&x, xts_enc(XI_addx, 0, 3, 4, 5)), XTS_EXEC_SIMULATED);
  HT_EQ_U64(x.out.gpr[3], 42);
  HT_EQ_U64(xts_exec_word(&x, 0), XTS_EXEC_SIMULATED);
  HT_CHECK(x.trap.trapped && x.trap.cause == XTS_CAUSE_ILLEGAL && x.trap.srr0 == xts_exec_slot_addr());
  HT_EQ_U64(xts_exec_word(&x, xts_enc(XI_mftb, 0, 3, 268)), XTS_EXEC_DRYRUN);   /* mftb: never modelled (nondeterministic) */
  xts_set_ptr(&x, 6, 128);
  HT_EQ_U64(x.in.gpr[6], xts_scratch_addr() + 128);
  xts_set_mem_ptr(&x, 16, 64, 0);
  HT_EQ_U64(xts_be_load(x.mem_in + 16, 8), xts_scratch_addr() + 64);
  HT_EQ_U64(xts_set_code_ptr(&x, 9, XTS_CODE_KERNEL, 8), 0);
  HT_CHECK(xts_set_code_ptr(&x, 30, XTS_CODE_KERNEL, 8) < 0);    /* r30 is reserved */
  HT_EQ_U64(xts_exec_word(&x, 0x60000000), XTS_EXEC_NOTRUN);     /* a setup error poisons the case */
  xts_case_init(&x);
  xts_kbuf_t *k = xk_new(64);
  xk_emit(k, XI_addx, 0, 3, 3, 4);
  xk_exit(k);
  xk_finish(k);
  HT_EQ_U64(xts_exec_kernel(&x, k), XTS_EXEC_DRYRUN);
  k->tag[0] = XI_orx;
  HT_EQ_U64(xts_exec_kernel(&x, k), XTS_EXEC_BADKERNEL);
  xk_free(k);
  uint64_t a;
  HT_CHECK(xts_big(1 << 20, &a) != NULL);
  HT_EQ_U64(xts_prep(0, 1024, 64, 0, 2, 3, 0), 0);
  HT_CHECK(xts_prep(0, 1 << 20, 64, 0, 2, 3, 0) < 0);
}

HTEST(state_compare)
{
  static xts_state_t a, b;
  xts_state_baseline(&a);
  b = a;
  b.gpr[30] = 5;                /* reserved: ignored */
  b.fpscr |= 0xFFFF000000000000ull;   /* high half: ignored */
  b.outptr = 1;
  HT_EQ_U64(xts_state_compare(&a, &b, NULL, 0), 0);
  b.vr[100].w[3] ^= 1;
  b.cr = 0x10000000;
  xts_state_diff_t d[4];
  HT_EQ_U64(xts_state_compare(&a, &b, d, 4), 2);
  HT_CHECK(!strcmp(d[0].what, "vr100") && !strcmp(d[1].what, "cr"));
  xts_vr_t v = xts_vr4(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  HT_EQ_U64(xts_vr_u8(&v, 5), 5);
  HT_EQ_U64(xts_vr_u16(&v, 3), 0x0607);
  uint8_t m[16];
  xts_vr_store(m, &v);
  HT_EQ_U64(m[15], 15);
}
