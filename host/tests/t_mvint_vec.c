/* Hand-computed vectors for the vector integer model (src/model/vint.c, vperm.c). Vectors marked [hw] are results4
 * cases recorded on a console. */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"

static xts_mres_t run(uint32_t w, xts_state_t *in, xts_state_t *out)
{
  xts_mem_t m;
  xts_mem_init(&m);
  return xts_model_exec(NULL, w, in, &m, out);
}

#define HT_VR(v, a, b, c, d) do { HT_EQ_U64((v).w[0], (a)); HT_EQ_U64((v).w[1], (b)); HT_EQ_U64((v).w[2], (c)); HT_EQ_U64((v).w[3], (d)); } while (0)

static xts_vr_t bytes_inc(int start)
{
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = (uint8_t)(start + i);
  return xts_vr_load(b);
}

HTEST(mvint_vperm)
{
  xts_state_t in, out;
  xts_state_baseline(&in);
  in.vr[4] = bytes_inc(0x00);                 /* A: 00..0F */
  in.vr[5] = bytes_inc(0x10);                 /* B: 10..1F */
  const uint8_t c[16] = { 0x1F, 0x00, 0x0F, 0x10, 0x20, 0x21, 0x3F, 0x12, 5, 5, 5, 5, 5, 5, 5, 5 };   /* index uses 5 bits */
  in.vr[6] = xts_vr_load(c);
  HT_EQ_U64(run(xts_enc(XI_vperm, 0, 3, 4, 5, 6), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x1F000F10, 0x00011F12, 0x05050505, 0x05050505);
  HT_EQ_U64(memcmp(&out.vr[4], &in.vr[4], 16), 0);
  /* vperm128: VD 7 bits, VA/VB 7 bits, VC only v0-v7 */
  in.vr[100] = bytes_inc(0x00);
  in.vr[101] = bytes_inc(0x10);
  in.vr[3] = xts_vr_load(c);
  HT_EQ_U64(run(xts_enc(XI_vperm128, 0, 70, 100, 101, 3), &in, &out), XTS_M_OK);
  HT_VR(out.vr[70], 0x1F000F10, 0x00011F12, 0x05050505, 0x05050505);
  HT_EQ_U64(out.vr[7].w[0], in.vr[7].w[0]);
  /* vsldoi / vsldoi128: bytes 5..20 of A||B */
  HT_EQ_U64(run(xts_enc(XI_vsldoi, 0, 3, 4, 5, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x05060708, 0x090A0B0C, 0x0D0E0F10, 0x11121314);
  HT_EQ_U64(run(xts_enc(XI_vsldoi128, 0, 70, 100, 101, 15), &in, &out), XTS_M_OK);
  HT_VR(out.vr[70], 0x0F101112, 0x13141516, 0x1718191A, 0x1B1C1D1E);
}

HTEST(mvint_pixel_pack)
{
  xts_state_t in, out;
  xts_state_baseline(&in);
  in.vr[4] = xts_vr4(0x01FF8008, 0xFE000000, 0x00F8F8F8, 0x01000000);
  in.vr[5] = xts_vr4(0x00080808, 0, 0, 0x01070707);
  HT_EQ_U64(run(xts_enc(XI_vpkpx, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  /* word bit 7 (LSB of the first byte) -> 1, then the top 5 bits of each remaining byte */
  HT_VR(out.vr[3], 0xFE010000, 0x7FFF8000, 0x04210000, 0x00008000);
  in.vr[5] = xts_vr4(0x80007FFF, 0x0421FC00, 0x8000FFFF, 0x00010002);
  HT_EQ_U64(run(xts_enc(XI_vupkhpx, 0, 3, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0xFF000000, 0x001F1F1F, 0x00010101, 0xFF1F0000);
  HT_EQ_U64(run(xts_enc(XI_vupklpx, 0, 3, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0xFF000000, 0xFF1F1F1F, 0x00000001, 0x00000002);
  /* unpack signed: high half = elements 0-7 (bytes) / 0-3 (halfwords); the 128 forms read VB128 */
  in.vr[5] = xts_vr4(0x7F80FF01, 0x00000000, 0x80000000, 0x0000FFFF);
  HT_EQ_U64(run(xts_enc(XI_vupkhsb, 0, 3, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x007FFF80, 0xFFFF0001, 0, 0);
  HT_EQ_U64(run(xts_enc(XI_vupklsb, 0, 3, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0xFF800000, 0, 0, 0xFFFFFFFF);
  in.vr[100] = in.vr[5];
  HT_EQ_U64(run(xts_enc(XI_vupkhsh128, 0, 70, 100), &in, &out), XTS_M_OK);
  HT_VR(out.vr[70], 0x00007F80, 0xFFFFFF01, 0, 0);
  HT_EQ_U64(run(xts_enc(XI_vupklsh128, 0, 70, 100), &in, &out), XTS_M_OK);
  HT_VR(out.vr[70], 0xFFFF8000, 0, 0, 0xFFFFFFFF);
}

HTEST(mvint_shift_counts)
{
  xts_state_t in, out;
  xts_state_baseline(&in);
  in.vr[4] = xts_vr4(0x12345678, 0x9ABCDEF0, 0x0F1E2D3C, 0x4B5A6978);
  in.vr[5] = xts_vr4(0xFCFCFCFC, 0xFCFCFCFC, 0xFCFCFCFC, 0xFCFCFCFC);          /* every byte & 7 == 4 */
  HT_EQ_U64(run(xts_enc(XI_vsl, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x23456789, 0xABCDEF00, 0xF1E2D3C4, 0xB5A69780);
  HT_EQ_U64(run(xts_enc(XI_vsr, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x01234567, 0x89ABCDEF, 0x00F1E2D3, 0xC4B5A697);
  /* [hw] unequal counts: every byte uses its OWN count (vB byte & 7), not byte 15's */
  in.vr[4] = xts_vr4(0x7F7F7F7F, 0x7F7F7F7F, 0x7F7F7F7F, 0x7F7F7F7F);
  in.vr[5] = xts_vr4(0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);
  HT_EQ_U64(run(xts_enc(XI_vsl, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0xBF7FBF7F, 0xBF7FBF7F, 0xBF7FBF7F, 0xBF7FBF7F);
  in.vr[4] = xts_vr4(0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);
  in.vr[5] = xts_vr4(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
  HT_EQ_U64(run(xts_enc(XI_vsr, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x01FE01FE, 0x01FE01FE, 0x01FE01FE, 0x01FE01FE);
  /* vslo / vsro: octet count = vB byte 15 bits 3-6 */
  in.vr[4] = bytes_inc(0);
  in.vr[5] = xts_vr4(0, 0, 0, 0x000000C1);              /* (0xC1 >> 3) & 0xF = 8 */
  HT_EQ_U64(run(xts_enc(XI_vslo, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x08090A0B, 0x0C0D0E0F, 0, 0);
  HT_EQ_U64(run(xts_enc(XI_vsro, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0, 0, 0x00010203, 0x04050607);
  HT_EQ_U64(run(xts_enc(XI_vsro128, 0, 70, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[70], 0, 0, 0x00010203, 0x04050607);
}

HTEST(mvint_vsel128)
{
  xts_state_t in, out;
  xts_state_baseline(&in);
  in.vr[70] = xts_vr4(0xFF00FF00, 0xFFFFFFFF, 0, 0x0F0F0F0F);      /* VD128 is the mask and the destination */
  in.vr[5] = xts_vr4(0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA);
  in.vr[100] = xts_vr4(0x55555555, 0x55555555, 0x55555555, 0x55555555);
  HT_EQ_U64(run(xts_enc(XI_vsel128, 0, 70, 5, 100, 70), &in, &out), XTS_M_OK);   /* D = (A & ~D) | (B & D) */
  HT_VR(out.vr[70], 0x55AA55AA, 0x55555555, 0xAAAAAAAA, 0xA5A5A5A5);
  /* vsel: D = (A & ~C) | (B & C) */
  in.vr[6] = xts_vr4(0xFF00FF00, 0xFFFFFFFF, 0, 0x0F0F0F0F);
  in.vr[7] = in.vr[100];
  HT_EQ_U64(run(xts_enc(XI_vsel, 0, 3, 5, 7, 6), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x55AA55AA, 0x55555555, 0xAAAAAAAA, 0xA5A5A5A5);
}

HTEST(mvint_sat_and_cr6)
{
  xts_state_t in, out;
  xts_state_baseline(&in);
  in.vr[4] = xts_vr4(0x7FFF8000, 0x00010002, 0x7FFF7FFF, 0xFFFF0000);
  in.vr[5] = xts_vr4(0x00010002, 0x00030004, 0x80008000, 0x00000000);
  /* vpkshss: signed halfwords -> signed bytes, saturating; A = elements 0-7, B = 8-15 */
  HT_EQ_U64(run(xts_enc(XI_vpkshss, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0x7F800102, 0x7F7FFF00, 0x01020304, 0x80800000);
  HT_EQ_U64(out.vscr.w[3] & XTS_VSCR_SAT, XTS_VSCR_SAT);
  /* no saturation: SAT stays clear */
  in.vr[4] = xts_vr4(0x00010002, 0, 0, 0);
  in.vr[5] = xts_vr4(0, 0, 0, 0);
  HT_EQ_U64(run(xts_enc(XI_vpkuhum, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_EQ_U64(out.vscr.w[3] & XTS_VSCR_SAT, 0);
  /* sticky: an existing SAT is kept */
  in.vscr = xts_vr4(0, 0, 0, XTS_VSCR_SAT);
  HT_EQ_U64(run(xts_enc(XI_vpkuhum, 0, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_EQ_U64(out.vscr.w[3] & XTS_VSCR_SAT, XTS_VSCR_SAT);
  in.vscr = xts_vr4(0, 0, 0, 0);
  /* Rc compare: CR6 = all-true<<3 | all-false<<1 */
  in.vr[4] = xts_vr4(1, 2, 3, 4);
  in.vr[5] = xts_vr4(1, 2, 3, 4);
  HT_EQ_U64(run(xts_enc(XI_vcmpequw, XTS_RC, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[3], 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
  HT_EQ_U64(xm_cr_field(&out, 6), 8);
  in.vr[5] = xts_vr4(9, 9, 9, 9);
  HT_EQ_U64(run(xts_enc(XI_vcmpequw128, XTS_RC, 70, 4, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vr[70], 0, 0, 0, 0);
  HT_EQ_U64(xm_cr_field(&out, 6), 2);
  in.vr[5] = xts_vr4(1, 9, 3, 9);
  HT_EQ_U64(run(xts_enc(XI_vcmpequw, XTS_RC, 3, 4, 5), &in, &out), XTS_M_OK);
  HT_EQ_U64(xm_cr_field(&out, 6), 0);
  HT_EQ_U64(run(xts_enc(XI_vcmpequw, 0, 3, 4, 5), &in, &out), XTS_M_OK);     /* no Rc: CR untouched */
  HT_EQ_U64(out.cr, in.cr);
  /* mtvscr keeps NJ/SAT of word 3 only, mfvscr returns it */
  in.vr[5] = xts_vr4(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
  HT_EQ_U64(run(xts_enc(XI_mtvscr, 0, 5), &in, &out), XTS_M_OK);
  HT_VR(out.vscr, 0, 0, 0, XTS_VSCR_NJ | XTS_VSCR_SAT);
  in.vscr = xts_vr4(0, 0, 0, XTS_VSCR_NJ);
  HT_EQ_U64(run(xts_enc(XI_mfvscr, 0, 8), &in, &out), XTS_M_OK);
  HT_VR(out.vr[8], 0, 0, 0, XTS_VSCR_NJ);
}
