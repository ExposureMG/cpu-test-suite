/* encoder / decoder / kernel builder */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

HTEST(encoder_roundtrip)
{
  xts_rng_t r;
  xts_rng_seed(&r, 42);
  int bad = 0;
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    for (int k = 0; k < 64; k++) {
      int64_t ops[XTS_MAX_OPS];
      int vd = -1;
      for (int i = 0; i < d->nops; i++) {
        const xts_field_t *f = &xts_fields[d->ops[i].field];
        int64_t lo = (f->kind == XTS_FK_SIGNED || f->kind == XTS_FK_DISP4) ? -((int64_t)1 << (f->bits - 1)) : 0;
        int64_t hi = (f->kind == XTS_FK_SIGNED || f->kind == XTS_FK_DISP4) ? ((int64_t)1 << (f->bits - 1)) - 1 : ((int64_t)1 << f->bits) - 1;
        ops[i] = xts_rng_range(&r, lo, hi);
        if (f->kind == XTS_FK_DISP4) ops[i] &= ~3;
        if (d->ops[i].field == XTS_F_VD128) vd = i;
      }
      for (int i = 0; i < d->nops; i++)
        if (d->ops[i].field == XTS_F_VS128 && vd >= 0) ops[i] = ops[vd];
      unsigned fl = (unsigned)xts_rng_next(&r) & d->flags;
      uint32_t w;
      int e = xts_encode(id, fl, ops, &w, NULL);
      if (e) { bad++; if (bad < 4) printf("    encode %s: %s\n", d->mnem, xts_enc_strerror(e)); continue; }
      if (!xts_decodes_as(w, id)) { bad++; if (bad < 4) printf("    %s %08X does not decode back\n", d->mnem, w); continue; }
      for (int i = 0; i < d->nops; i++)
        if (xts_extract(id, w, i) != ops[i]) { bad++; if (bad < 4) printf("    %s op %d\n", d->mnem, i); }
      if (xts_flags_of(id, w) != fl) { bad++; if (bad < 4) printf("    %s flags %X vs %X\n", d->mnem, xts_flags_of(id, w), fl); }
    }
  }
  HT_EQ_U64(bad, 0);
  /* known words */
  HT_EQ_U64(xts_enc(XI_addx, XTS_OE | XTS_RC, 3, 4, 5), 0x7C642E15);
  HT_EQ_U64(xts_enc(XI_orx, 0, 1, 1, 1), 0x7C210B78);
  HT_EQ_U64(xts_enc(XI_tw, 0, 31, 0, 0), 0x7FE00008);
  HT_EQ_U64(xts_enc(XI_ori, 0, 0, 0, 0), 0x60000000);
  HT_EQ_U64(xts_enc(XI_vor128, 0, 100, 101, 127), 0x1485FEFF);
  HT_EQ_U64(xts_enc(XI_lvx128, 0, 64, 3, 4), 0x100320CB);
  HT_EQ_U64(xts_enc(XI_vmaddfp128, 0, 1, 2, 3, 1), 0x142218D0);
  HT_EQ_U64(xts_enc(XI_rldiclx, 0, 3, 4, 40, 10), 0x78834282);
  HT_EQ_U64(xts_enc(XI_mfspr, 0, 3, 1008), 0x7C70FAA6);
  HT_EQ_U64(xts_enc(XI_sc, 0, 1), 0x44000022);
  /* errors */
  HT_EQ_U64(xts_enc(XI_addx, 0, 32, 0, 0), 0);
  HT_EQ_U64(xts_enc_last_error, XTS_ENC_RANGE);
  HT_EQ_U64(xts_enc(XI_vmaddfp128, 0, 1, 2, 3, 4), 0);
  HT_EQ_U64(xts_enc_last_error, XTS_ENC_TIED);
  HT_EQ_U64(xts_enc(XI_addx, XTS_LK, 1, 2, 3), 0);
  HT_EQ_U64(xts_enc_last_error, XTS_ENC_BAD_FLAGS);
  HT_EQ_U64(xts_enc(XI_addx, 0, 1, 2), 0);
  HT_EQ_U64(xts_enc_last_error, XTS_ENC_NOPS);
  HT_EQ_U64(xts_enc(XI_ld, 0, 3, 6, 4), 0);          /* ds must be a multiple of 4 */
  HT_EQ_U64(xts_enc_last_error, XTS_ENC_RANGE);
  /* lookups and text */
  HT_EQ_U64(xts_isa_find("addx"), XI_addx);
  HT_EQ_U64(xts_isa_find("add"), XI_addx);
  HT_EQ_U64(xts_decode1(0), (uint64_t)-1);
  char buf[64];
  HT_CHECK(!strcmp(xts_disasm(0x7C642E15, buf, sizeof buf), "addo. 3, 4, 5"));
  int illegal = 0;
  for (int i = 0; i < xts_isa_count; i++) illegal += xts_isa[i].hw == XTS_HW_ILLEGAL;
  HT_EQ_U64(illegal, 29);
  double lat;
  HT_CHECK(xts_isa_baseline(XI_addx, "latency", &lat) && lat > 1.5 && lat < 2.5);
}

HTEST(kernel_builder)
{
  xts_kbuf_t *k = xk_new(256);
  HT_CHECK(k != NULL);
  xk_emit(k, XI_addi, 0, 9, 0, 10);
  int top = xk_loop_begin(k, 9);
  xk_emit(k, XI_addx, 0, 3, 3, 4);
  int skip = xk_label(k);
  xk_bc(k, 12, 2, skip);
  xk_nop(k);
  xk_bind(k, skip);
  xk_loop_end(k, top);
  xk_exit(k);
  HT_EQ_U64(xk_finish(k), 0);
  int ni, nr;
  char why[96];
  HT_EQ_U64(xk_validate(k, &ni, &nr, why, sizeof why), 0);
  HT_EQ_U64(nr, 1);
  /* li; mtctr; top: add; beq skip; nop; skip: bdnz top; exit */
  HT_EQ_U64(k->w[5], 0x4200FFF4);       /* bdnz -12 */
  HT_EQ_U64(k->w[3], 0x41820008);       /* beq +8 over the nop */
  k->tag[2] = XI_orx;                    /* lie about one word: validation must catch it */
  HT_CHECK(xk_validate(k, &ni, &nr, why, sizeof why) == 1);
  xk_reset(k);
  xk_emit(k, XI_addx, 0, 3, 3, 99);      /* bad operand: error recorded, later emits ignored */
  HT_CHECK(k->err != 0);
  HT_CHECK(xk_finish(k) < 0);
  xk_reset(k);
  xk_emit(k, XI_vmulouh, 0, 1, 2, 3);    /* hardware-illegal as an instruction: validation flags it */
  xk_exit(k);
  xk_finish(k);
  HT_CHECK(xk_validate(k, &ni, &nr, why, sizeof why) == 1);
  xk_reset(k);
  xk_data(k, 0x10000408);                /* the same word as deliberate data: fine */
  xk_exit(k);
  xk_finish(k);
  HT_CHECK(xk_validate(k, &ni, &nr, why, sizeof why) == 0);
  xk_free(k);
}
