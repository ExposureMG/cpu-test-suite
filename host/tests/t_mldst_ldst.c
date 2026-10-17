/* load/store model (src/model/ldst*.c, cellvmx.c): hand vectors for the paths results4 does not cover (lmw/stmw, string
 * ops, SF=0 addressing, reservation sequences, invalid forms) and spot checks of the ones it does. */
#include <string.h>
#include <xts/xts.h>
#include <model/model_priv.h>   /* xts_model_resv_clear */
#include "htest.h"

#define BASE 0x80001000ull

static uint8_t mem[512] __attribute__((aligned(128)));
static xts_mem_t M;
static xts_state_t S, O;

static void setup(void)
{
  for (int i = 0; i < 512; i++) mem[i] = (uint8_t)i;
  xts_mem_init(&M);
  xts_mem_add(&M, BASE, mem, sizeof mem);
  xts_state_baseline(&S);
  S.gpr[3] = BASE;
  S.xer = 0;
  S.cr = 0;
  xts_model_resv_clear();
}

static xts_mres_t run(uint32_t w) { return xts_model_exec(NULL, w, &S, &M, &O); }

HTEST(mldst_lmw_stmw)
{
  setup();
  /* lmw r28,8(r3): r28..r31 = words at +8,+12,+16,+20, upper halves cleared */
  HT_EQ_U64(run(xts_enc(XI_lmw, 0, 28, 8, 3)), XTS_M_OK);
  HT_EQ_U64(O.gpr[28], 0x08090A0B);
  HT_EQ_U64(O.gpr[29], 0x0C0D0E0F);
  HT_EQ_U64(O.gpr[30], 0x10111213);
  HT_EQ_U64(O.gpr[31], 0x14151617);
  HT_EQ_U64(O.gpr[27], S.gpr[27]);
  /* RA in the loaded range is the invalid form (also RA=0 with RT=0); misaligned EA -> alignment */
  HT_EQ_U64(run(xts_enc(XI_lmw, 0, 3, 0, 3)), XTS_M_ILLEGAL);
  HT_EQ_U64(run(xts_enc(XI_lmw, 0, 31, 2, 3)), XTS_M_ALIGNMENT);
  HT_EQ_U64(run(xts_enc(XI_lmw, 0, 31, 0x7FF0, 3)), XTS_M_DSI);
  /* lmw r31 with a huge range crossing the end of memory */
  /* stmw r29,0x20(r3): 3 words from the low halves */
  S.gpr[29] = 0xAAAAAAAA11223344ull;
  S.gpr[30] = 0x55667788;
  S.gpr[31] = 0xFFFFFFFFDEADBEEFull;
  HT_EQ_U64(run(xts_enc(XI_stmw, 0, 29, 0x20, 3)), XTS_M_OK);
  HT_EQ_U64(xts_be_load(mem + 0x20, 4), 0x11223344);
  HT_EQ_U64(xts_be_load(mem + 0x24, 4), 0x55667788);
  HT_EQ_U64(xts_be_load(mem + 0x28, 4), 0xDEADBEEF);
  HT_EQ_U64(mem[0x2C], 0x2C);
  HT_EQ_U64(memcmp(O.gpr, S.gpr, sizeof S.gpr), 0);
}

HTEST(mldst_string)
{
  setup();
  /* lswi r5,r3,6: r5 = 00010203, r6 = 04050000 (unfilled low bytes zero), upper halves zero */
  S.gpr[5] = S.gpr[6] = ~0ull;
  HT_EQ_U64(run(xts_enc(XI_lswi, 0, 5, 3, 6)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0x00010203);
  HT_EQ_U64(O.gpr[6], 0x04050000);
  HT_EQ_U64(O.gpr[7], S.gpr[7]);
  /* NB=0 means 32 bytes, wrapping r31 -> r0: lswi r28,r10,0 loads r28..r31,r0..r3 (RA=r10 is outside the range) */
  S.gpr[10] = BASE + 0x40;
  HT_EQ_U64(run(xts_enc(XI_lswi, 0, 28, 10, 0)), XTS_M_OK);
  HT_EQ_U64(O.gpr[28], 0x40414243);
  HT_EQ_U64(O.gpr[31], 0x4C4D4E4F);
  HT_EQ_U64(O.gpr[0], 0x50515253);
  HT_EQ_U64(O.gpr[3], 0x5C5D5E5F);
  /* RA inside the loaded range: invalid form */
  HT_EQ_U64(run(xts_enc(XI_lswi, 0, 2, 3, 8)), XTS_M_ILLEGAL);
  /* lswx: byte count from XER[25:31] (other XER bits ignored); RT=RB invalid */
  S.gpr[4] = 3;
  S.xer = 0x7FFFFF80ull | 5;
  HT_EQ_U64(run(xts_enc(XI_lswx, 0, 8, 3, 4)), XTS_M_OK);
  HT_EQ_U64(O.gpr[8], 0x03040506);
  HT_EQ_U64(O.gpr[9], 0x07000000);
  HT_EQ_U64(run(xts_enc(XI_lswx, 0, 4, 3, 4)), XTS_M_ILLEGAL);
  S.xer = 0;                                              /* n = 0: nothing happens */
  HT_EQ_U64(run(xts_enc(XI_lswx, 0, 8, 3, 4)), XTS_M_OK);
  HT_EQ_U64(memcmp(O.gpr, S.gpr, sizeof S.gpr), 0);
  /* stswx r12,r3,r4 with n=6 at BASE+3: bytes from the low words of r12, r13 */
  S.gpr[12] = 0xFFFFFFFF01020304ull;
  S.gpr[13] = 0x0000000005060708ull;
  S.xer = 6;
  HT_EQ_U64(run(xts_enc(XI_stswx, 0, 12, 3, 4)), XTS_M_OK);
  HT_EQ_U64(xts_be_load(mem + 3, 4), 0x01020304);
  HT_EQ_U64(xts_be_load(mem + 7, 2), 0x0506);
  HT_EQ_U64(mem[9], 9);
  /* stswi r30,r3,5 wraps nothing but uses r30,r31 */
  S.gpr[30] = 0xA1A2A3A4;
  S.gpr[31] = 0xB1B2B3B4;
  HT_EQ_U64(run(xts_enc(XI_stswi, 0, 30, 3, 5)), XTS_M_OK);
  HT_EQ_U64(xts_be_load(mem, 4), 0xA1A2A3A4);
  HT_EQ_U64(mem[4], 0xB1);
  HT_EQ_U64(mem[5], 3);                              /* left over from the stswx above */
}

static void vec_bytes(xts_state_t *s, int vr, int base)
{
  uint8_t q[16];
  for (int i = 0; i < 16; i++) q[i] = (uint8_t)(base + i);
  s->vr[vr] = xts_vr_load(q);
}
static int vr_byte(const xts_state_t *s, int vr, int i) { return xts_vr_u8(&s->vr[vr], i); }

HTEST(mldst_cell_lvlx_lvrx)
{
  setup();
  S.gpr[4] = 0x23;                                        /* EA = BASE+0x23, EA & 15 = 3 */
  vec_bytes(&S, 7, 0xA0);
  /* lvlx v6,r3,r4: memory bytes 0x23..0x2F in the high end, zero below */
  HT_EQ_U64(run(xts_enc(XI_lvlx, 0, 6, 3, 4)), XTS_M_OK);
  for (int i = 0; i < 13; i++) HT_EQ_U64(vr_byte(&O, 6, i), 0x23 + i);
  for (int i = 13; i < 16; i++) HT_EQ_U64(vr_byte(&O, 6, i), 0);
  /* lvrx v6,r3,r4: memory bytes 0x20..0x22 in the low end, zero above */
  HT_EQ_U64(run(xts_enc(XI_lvrx, 0, 6, 3, 4)), XTS_M_OK);
  for (int i = 0; i < 13; i++) HT_EQ_U64(vr_byte(&O, 6, i), 0);
  HT_EQ_U64(vr_byte(&O, 6, 13), 0x20);
  HT_EQ_U64(vr_byte(&O, 6, 15), 0x22);
  /* aligned EA: lvlx loads the whole quadword, lvrx gives zero (and touches no memory) */
  S.gpr[4] = 0x30;
  HT_EQ_U64(run(xts_enc(XI_lvlxl, 0, 6, 3, 4)), XTS_M_OK);
  for (int i = 0; i < 16; i++) HT_EQ_U64(vr_byte(&O, 6, i), 0x30 + i);
  HT_EQ_U64(run(xts_enc(XI_lvrxl, 0, 6, 3, 4)), XTS_M_OK);
  for (int i = 0; i < 16; i++) HT_EQ_U64(vr_byte(&O, 6, i), 0);
  /* stvlx v7: vS bytes 0..12 -> memory 0x23..0x2F */
  S.gpr[4] = 0x23;
  HT_EQ_U64(run(xts_enc(XI_stvlx, 0, 7, 3, 4)), XTS_M_OK);
  HT_EQ_U64(mem[0x22], 0x22);
  HT_EQ_U64(mem[0x23], 0xA0);
  HT_EQ_U64(mem[0x2F], 0xAC);
  HT_EQ_U64(mem[0x30], 0x30);
  /* stvrx v7: vS bytes 13..15 -> memory 0x20..0x22 */
  HT_EQ_U64(run(xts_enc(XI_stvrx, 0, 7, 3, 4)), XTS_M_OK);
  HT_EQ_U64(mem[0x1F], 0x1F);
  HT_EQ_U64(mem[0x20], 0xAD);
  HT_EQ_U64(mem[0x22], 0xAF);
  HT_EQ_U64(mem[0x23], 0xA0);              /* stvrx did not touch it */
  /* VMX128 forms use the 7-bit register fields */
  vec_bytes(&S, 100, 0x50);
  HT_EQ_U64(run(xts_enc(XI_stvlx128, 0, 100, 3, 4)), XTS_M_OK);
  HT_EQ_U64(mem[0x23], 0x50);
  HT_EQ_U64(run(xts_enc(XI_lvlx128, 0, 101, 3, 4)), XTS_M_OK);
  HT_EQ_U64(vr_byte(&O, 101, 0), 0x50);
  HT_EQ_U64(vr_byte(&O, 101, 13), 0);
  HT_EQ_U64(run(xts_enc(XI_lvrx128, 0, 127, 3, 4)), XTS_M_OK);
  HT_EQ_U64(vr_byte(&O, 127, 13), 0xAD);        /* what stvrx left at 0x20 */
}

HTEST(mldst_altivec_mem)
{
  setup();
  S.gpr[4] = 0x35;
  /* lvx ignores the low 4 bits; lvebx/lvehx/lvewx load the whole quadword on Xenon */
  HT_EQ_U64(run(xts_enc(XI_lvx, 0, 2, 3, 4)), XTS_M_OK);
  HT_EQ_U64(vr_byte(&O, 2, 0), 0x30);
  HT_EQ_U64(run(xts_enc(XI_lvebx, 0, 2, 3, 4)), XTS_M_OK);
  HT_EQ_U64(vr_byte(&O, 2, 15), 0x3F);
  /* lvsl / lvsr */
  HT_EQ_U64(run(xts_enc(XI_lvsl, 0, 2, 3, 4)), XTS_M_OK);
  HT_EQ_U64(vr_byte(&O, 2, 0), 5);
  HT_EQ_U64(vr_byte(&O, 2, 15), 20);
  HT_EQ_U64(run(xts_enc(XI_lvsr, 0, 2, 3, 4)), XTS_M_OK);
  HT_EQ_U64(vr_byte(&O, 2, 0), 11);
  HT_EQ_U64(vr_byte(&O, 2, 15), 26);
  /* element stores: byte at EA, halfword at EA&~1, word at EA&~3, taken from that element position */
  vec_bytes(&S, 9, 0xC0);
  HT_EQ_U64(run(xts_enc(XI_stvebx, 0, 9, 3, 4)), XTS_M_OK);
  HT_EQ_U64(mem[0x35], 0xC5);
  HT_EQ_U64(mem[0x34], 0x34);
  HT_EQ_U64(run(xts_enc(XI_stvehx, 0, 9, 3, 4)), XTS_M_OK);
  HT_EQ_U64(xts_be_load(mem + 0x34, 2), 0xC4C5);
  HT_EQ_U64(run(xts_enc(XI_stvewx, 0, 9, 3, 4)), XTS_M_OK);
  HT_EQ_U64(xts_be_load(mem + 0x34, 4), 0xC4C5C6C7);
  HT_EQ_U64(run(xts_enc(XI_stvx, 0, 9, 3, 4)), XTS_M_OK);
  HT_EQ_U64(mem[0x30], 0xC0);
  HT_EQ_U64(mem[0x3F], 0xCF);
  HT_EQ_U64(run(xts_enc(XI_stvewx128, 0, 120, 3, 4)), XTS_M_OK);
}

static uint64_t ld_fp(uint32_t single)
{
  setup();
  xts_be_store(mem, 4, single);
  S.gpr[4] = 0;
  xts_mres_t r = run(xts_enc(XI_lfsx, 0, 5, 3, 4));
  return r == XTS_M_OK ? O.fpr[5] : 0xBADBADBADull;
}
static uint32_t st_fp(uint64_t dbl)
{
  setup();
  S.fpr[5] = dbl;
  S.gpr[4] = 0;
  xts_mres_t r = run(xts_enc(XI_stfsx, 0, 5, 3, 4));
  return r == XTS_M_OK ? (uint32_t)xts_be_load(mem, 4) : 0xBADBAD;
}

HTEST(mldst_fp_conversion)
{
  HT_EQ_U64(ld_fp(0x3F800000), 0x3FF0000000000000ull);            /* 1.0 */
  HT_EQ_U64(ld_fp(0x80000000), 0x8000000000000000ull);            /* -0 */
  HT_EQ_U64(ld_fp(0x00400000), 0x3800000000000000ull);            /* 2^-127: denormal single, normalised */
  HT_EQ_U64(ld_fp(0x00000001), 0x36A0000000000000ull);            /* 2^-149 */
  HT_EQ_U64(ld_fp(0x007FFFFF), 0x380FFFFFC0000000ull);
  HT_EQ_U64(ld_fp(0x7F800000), 0x7FF0000000000000ull);            /* +inf */
  HT_EQ_U64(ld_fp(0xFFC00000), 0xFFF8000000000000ull);
  HT_EQ_U64(ld_fp(0x7F800001), 0x7FF0000020000000ull);            /* SNaN stays signalling (not quieted) */
  HT_EQ_U64(ld_fp(0x7F7FFFFF), 0x47EFFFFFE0000000ull);            /* FLT_MAX */
  HT_EQ_U64(st_fp(0x3FF0000000000000ull), 0x3F800000);
  HT_EQ_U64(st_fp(0xBFF8000000000000ull), 0xBFC00000);
  HT_EQ_U64(st_fp(0x3800000000000000ull), 0x00400000);            /* 2^-127: denormalised by truncation */
  HT_EQ_U64(st_fp(0x36A0000000000000ull), 0x00000001);            /* 2^-149 */
  HT_EQ_U64(st_fp(0x369FFFFFFFFFFFFFull), 0x00000000);            /* below 874: undefined, model stores +0 */
  HT_EQ_U64(st_fp(0x3810000000000000ull), 0x00800000);            /* 2^-126, a normal single */
  HT_EQ_U64(st_fp(0x3810000000000001ull), 0x00800000);            /* low 29 fraction bits dropped, no rounding */
  HT_EQ_U64(st_fp(0x47F0000000000000ull), 0x7F800000);            /* 2^128: bit shuffle, not "rounded to inf" */
  HT_EQ_U64(st_fp(0x7FF0000000000000ull), 0x7F800000);
  HT_EQ_U64(st_fp(0x7FF4000000000000ull), 0x7FA00000);            /* SNaN payload kept */
  HT_EQ_U64(st_fp(0x0000000000000001ull), 0x00000000);
  HT_EQ_U64(st_fp(0x8000000000000000ull), 0x80000000);
}

HTEST(mldst_fp_access_rules)
{
  setup();
  S.fpr[5] = 0x1122334455667788ull;
  S.gpr[4] = 4;                                                    /* EA%8 == 4: fine */
  HT_EQ_U64(run(xts_enc(XI_stfdx, 0, 5, 3, 4)), XTS_M_OK);
  HT_EQ_U64(xts_be_load(mem + 4, 8), 0x1122334455667788ull);
  HT_EQ_U64(run(xts_enc(XI_stfiwx, 0, 5, 3, 4)), XTS_M_OK);        /* low word */
  HT_EQ_U64(xts_be_load(mem + 4, 4), 0x55667788);
  for (int r = 1; r < 8; r++) {
    S.gpr[4] = (uint64_t)r;
    HT_EQ_U64(run(xts_enc(XI_lfdx, 0, 6, 3, 4)), (r & 3) ? XTS_M_ALIGNMENT : XTS_M_OK);
    HT_EQ_U64(run(xts_enc(XI_stfsx, 0, 6, 3, 4)), (r & 3) ? XTS_M_ALIGNMENT : XTS_M_OK);
    HT_EQ_U64(run(xts_enc(XI_stfiwx, 0, 6, 3, 4)), (r & 3) ? XTS_M_ALIGNMENT : XTS_M_OK);
    HT_EQ_U64(run(xts_enc(XI_lwzx, 0, 7, 3, 4)), XTS_M_OK);        /* integer: never */
    HT_EQ_U64(run(xts_enc(XI_ldx, 0, 7, 3, 4)), XTS_M_OK);
    HT_EQ_U64(run(xts_enc(XI_lvx, 0, 7, 3, 4)), XTS_M_OK);
  }
  /* update form with RA=0 is the invalid form, RA=RT as well for integer loads, but stwu r3,..(r3) is fine */
  HT_EQ_U64(run(xts_enc(XI_lfdu, 0, 1, 8, 0)), XTS_M_ILLEGAL);
  HT_EQ_U64(run(xts_enc(XI_lwzu, 0, 0, 8, 0)), XTS_M_ILLEGAL);
  HT_EQ_U64(run(xts_enc(XI_lwzu, 0, 3, 8, 3)), XTS_M_ILLEGAL);
  HT_EQ_U64(run(xts_enc(XI_ldux, 0, 4, 4, 5)), XTS_M_ILLEGAL);
  HT_EQ_U64(run(xts_enc(XI_stwu, 0, 3, 8, 0)), XTS_M_ILLEGAL);
  S.gpr[3] = BASE + 0x40;
  HT_EQ_U64(run(xts_enc(XI_stwu, 0, 3, 8, 3)), XTS_M_OK);          /* RS=RA: stores the old value, RA := EA */
  HT_EQ_U64(xts_be_load(mem + 0x48, 4), (uint32_t)(BASE + 0x40));
  HT_EQ_U64(O.gpr[3], BASE + 0x48);
  HT_EQ_U64(run(xts_enc(XI_lwz, 0, 4, 0x7FF0, 3)), XTS_M_DSI);
}

HTEST(mldst_int_forms)
{
  setup();
  S.gpr[4] = 0x10;
  HT_EQ_U64(run(xts_enc(XI_lwbrx, 0, 5, 3, 4)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0x13121110);
  HT_EQ_U64(run(xts_enc(XI_lhbrx, 0, 5, 3, 4)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0x1110);
  HT_EQ_U64(run(xts_enc(XI_ldbrx, 0, 5, 3, 4)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0x1716151413121110ull);
  xts_be_store(mem + 0x10, 4, 0x80000001);
  HT_EQ_U64(run(xts_enc(XI_lwax, 0, 5, 3, 4)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0xFFFFFFFF80000001ull);
  HT_EQ_U64(run(xts_enc(XI_lwzx, 0, 5, 3, 4)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0x80000001ull);
  /* ld/lwa/std are DS-form: displacement in the word with the low 2 bits as opcode extension */
  HT_EQ_U64(run(xts_enc(XI_lwa, 0, 5, 0x10, 3)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0xFFFFFFFF80000001ull);
  S.gpr[6] = 0x0102030405060708ull;
  HT_EQ_U64(run(xts_enc(XI_stdbrx, 0, 6, 3, 4)), XTS_M_OK);
  HT_EQ_U64(xts_be_load(mem + 0x10, 8), 0x0807060504030201ull);
  HT_EQ_U64(run(xts_enc(XI_std, 0, 6, -8, 4)), XTS_M_DSI);
  /* MSR[SF]=0: the EA is the low 32 bits of the sum, update forms write that to RA */
  S.mode = 0;
  S.gpr[3] = BASE + (1ull << 32) + 0x20;
  HT_EQ_U64(run(xts_enc(XI_lbzu, 0, 5, 1, 3)), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0x21);
  HT_EQ_U64(O.gpr[3], BASE + 0x21);
  S.mode = XTS_MODE_SF;
  HT_EQ_U64(run(xts_enc(XI_lbzu, 0, 5, 1, 3)), XTS_M_DSI);
}

HTEST(mldst_reservation)
{
  setup();
  S.gpr[4] = 0x48;
  S.gpr[6] = 0xCAFEBABE12345678ull;
  uint32_t lw = xts_enc(XI_lwarx, 0, 5, 3, 4), sw = xts_enc(XI_stwcx, 0, 6, 3, 4);
  uint32_t ld = xts_enc(XI_ldarx, 0, 5, 3, 4), sd = xts_enc(XI_stdcx, 0, 6, 3, 4);
  S.xer = XTS_XER_SO;
  HT_EQ_U64(run(sw), XTS_M_OK);                                    /* no reservation: fails, CR0 = 0b0001 (SO) */
  HT_EQ_U64(xm_cr_field(&O, 0), 1);
  HT_EQ_U64(mem[0x48], 0x48);
  HT_EQ_U64(run(lw), XTS_M_OK);
  HT_EQ_U64(O.gpr[5], 0x48494A4B);
  HT_EQ_U64(run(sw), XTS_M_OK);                                    /* same line: succeeds, CR0 = EQ|SO */
  HT_EQ_U64(xm_cr_field(&O, 0), 3);
  HT_EQ_U64(xts_be_load(mem + 0x48, 4), 0x12345678);
  HT_EQ_U64(run(sw), XTS_M_OK);                                    /* the reservation was consumed */
  HT_EQ_U64(xm_cr_field(&O, 0), 1);
  S.xer = 0;
  S.gpr[4] = 0x48;
  run(lw);
  S.gpr[4] = 0x100;                                                /* other 128-byte line: fails, and clears */
  HT_EQ_U64(run(sw), XTS_M_OK);
  HT_EQ_U64(xm_cr_field(&O, 0), 0);
  S.gpr[4] = 0x48;
  HT_EQ_U64(run(sw), XTS_M_OK);
  HT_EQ_U64(xm_cr_field(&O, 0), 0);
  /* alignment: 4 for lwarx/stwcx., 8 for ldarx/stdcx.; a trapping larx leaves no reservation */
  S.gpr[4] = 0x84;
  HT_EQ_U64(run(lw), XTS_M_OK);
  HT_EQ_U64(run(ld), XTS_M_ALIGNMENT);
  HT_EQ_U64(run(sd), XTS_M_ALIGNMENT);
  S.gpr[4] = 0x82;
  HT_EQ_U64(run(lw), XTS_M_ALIGNMENT);
  HT_EQ_U64(run(sw), XTS_M_ALIGNMENT);
  S.gpr[4] = 0x88;
  HT_EQ_U64(run(ld), XTS_M_OK);
  HT_EQ_U64(run(sd), XTS_M_OK);
  HT_EQ_U64(xm_cr_field(&O, 0), 2);
  HT_EQ_U64(xts_be_load(mem + 0x88, 8), 0xCAFEBABE12345678ull);
  /* Rc=0 is an illegal form */
  HT_EQ_U64(run(sw & ~1u), XTS_M_ILLEGAL);
  HT_EQ_U64(run(sd & ~1u), XTS_M_ILLEGAL);
}

HTEST(mldst_dcbz_and_nops)
{
  setup();
  memset(mem, 0xFF, sizeof mem);
  S.gpr[4] = 0x95;
  HT_EQ_U64(run(xts_enc(XI_dcbz, 0, 3, 4)), XTS_M_OK);
  HT_EQ_U64(mem[0x7F], 0xFF);
  HT_EQ_U64(mem[0x80], 0);
  HT_EQ_U64(mem[0xFF], 0);
  HT_EQ_U64(mem[0x100], 0xFF);
  memset(mem, 0xFF, sizeof mem);
  HT_EQ_U64(run(xts_enc(XI_dcbz128, 0, 3, 4)), XTS_M_OK);
  HT_EQ_U64(mem[0x80], 0);
  HT_EQ_U64(mem[0xFF], 0);
  HT_EQ_U64(mem[0x7F], 0xFF);
  S.gpr[4] = 0x7FF0;
  HT_EQ_U64(run(xts_enc(XI_dcbz, 0, 3, 4)), XTS_M_DSI);
  static const int nops[] = { XI_sync, XI_eieio, XI_isync };
  for (unsigned i = 0; i < sizeof nops / sizeof nops[0]; i++) {
    HT_EQ_U64(run(xts_enc(nops[i], 0)), XTS_M_OK);
    HT_EQ_U64(memcmp(&O, &S, sizeof S), 0);
  }
  HT_EQ_U64(run(xts_enc(XI_dcbt, 0, 3, 4)), XTS_M_OK);
  HT_EQ_U64(run(xts_enc(XI_dcbf, 0, 3, 4)), XTS_M_OK);
  HT_EQ_U64(run(xts_enc(XI_icbi, 0, 3, 4)), XTS_M_OK);
  HT_EQ_U64(run(xts_enc(XI_xdcbt, 0, 3, 4)), XTS_M_OK);
  HT_EQ_U64(memcmp(&O, &S, sizeof S), 0);
}
