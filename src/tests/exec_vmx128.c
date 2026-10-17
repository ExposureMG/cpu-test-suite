/* exec.vmx128 - all 75 VMX128 instructions the hardware executes, v0..v127, CPU vs reference model.
 *
 * 70 % of the vector registers are drawn from v32..v127 (the high bits of the 7-bit fields VD128 / VA128 / VB128 are
 * assembled from scattered instruction bits, so a wrong placement shows as a wrong register), VC128 from v0..v7, and the
 * VMX128 tied operands (VD128 = VS128) are encoded with one register. Immediates cover their FULL encodable range:
 *   vpkd3d128      VD3D0 (type) 0..7 x VD3D1 (pack) 0..3 x VD3D2 (shift) 0..3   (128 combinations x 2 data draws)
 *   vupkd3d128     UIMM 0..31 (type = UIMM >> 2 = 0..7), 1 draw per value + 64 targeted draws
 *   vrlimi128      UIMM 0..31 (mask 0..15 and bit 4) x VD3D2 0..3 (rotate)       (128 x 2)
 *   vpermwi128     VPERM128 0..255                                                  (256 x 2)
 *   vsldoi128 SHB 0..15, vspltw128 / vcfpsxws128 / vcfpuxws128 / vcsxwfp128 / vcuxwfp128 UIMM 0..31, vspltisw128 SIMM -16..15
 * (the old hardware run only drew these at random, so combinations were missed). The memory forms use the same pointer /
 * index scheme as exec.altivec. The two hardware-illegal VMX128 words (lvsl128, lvsr128) are exec.illegal's.
 *
 * Targeted cases (src/model/KNOWN.md "Not covered by results4"), numbered after the base cases (".t" in the case id):
 *   vmaddfp128 / vmaddcfp128 / vnmsubfp128 (48 each): t%3 = 0 two or more different NaNs in a lane (QNaN / SNaN / negative),
 *        1 VSCR[NJ] forced to 1 with denormal results / inputs, 2 exact cancellation (addend = -(product), -0 for vnmsub).
 *   vmsum3fp128 / vmsum4fp128 (96 each): t%6 = 0 small products 20..100 bits below the largest of either sign, 1 x + (-x) exact
 *        zero sums, 2 all-zero products with signs, 3 product overflow with an in-range / cancelling sum, 4 infinity plus an
 *        overflowing finite sum, 5 denormal inputs with NJ forced to 1.
 *   vpkd3d128 (every base case): lanes drawn from D3D integer codes inside / outside the clamp window, NaNs (unsigned fields),
 *        half-denormal floats, FLOAT16 rounding-sensitive mantissas, colour range values.
 *   vupkd3d128: source words built from 16-bit halves {0x8000, FLOAT16 exp 0 / 31, ...} and 10 / 20-bit fields {0x200, ...}.
 * Cases that force NJ say so in their detail.note ("NJ forced"): the vscr_nj axis label of such a record is overridden. */
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "exec_common.h"

static int select_128(const xts_insn_t *d)
{
  return d->isa == XTS_ISA_VMX128 && d->priv == XTS_PRIV_USER && d->hw == XTS_HW_EXECUTES;
}

static int is_m(const xts_insn_t *d, const char *m) { return !strcmp(d->mnem, m); }

static int v128_extra(const xts_insn_t *d)
{
  if (is_m(d, "vmaddfp128") || is_m(d, "vmaddcfp128") || is_m(d, "vnmsubfp128")) return 48;
  if (is_m(d, "vmsum3fp128") || is_m(d, "vmsum4fp128")) return 96;
  if (is_m(d, "vupkd3d128")) return 64;
  return 0;
}

static int v128_enumerate(xts_caselist_t *cl) { return xe_vec_enumerate(cl, select_128, 2, v128_extra); }

static int opi(const xts_insn_t *d, int field)
{
  for (int i = 0; i < d->nops; i++)
    if (d->ops[i].field == field) return i;
  return -1;
}

static uint32_t fw(uint32_t s, uint32_t e, uint32_t m) { return (s << 31) | (e << 23) | (m & 0x7FFFFF); }
static uint32_t rm23(xts_rng_t *r) { return (uint32_t)xts_rng_next(r) & 0x7FFFFF; }
static uint32_t rbit(xts_rng_t *r) { return (uint32_t)xts_rng_below(r, 2); }
static float u2f(uint32_t u) { union { uint32_t u; float f; } a; a.u = u; return a.f; }
static uint32_t f2u(float f) { union { uint32_t u; float f; } a; a.f = f; return a.u; }

/* ---- FMA ---- */
static void fma_targeted(const xts_insn_t *d, xts_rng_t *r, xts_exec_t *x, const int64_t *ops, int t, const char **note)
{
  static const uint32_t nans[] = { 0x7FC00001, 0x7FC00002, 0x7F800001, 0x7F800002, 0xFFC00003, 0xFF800004, 0x7FFFFFFF, 0xFFFFFFFF };
  static const float small[] = { 1.0f, -1.0f, 1.5f, -1.5f, 2.0f, -2.0f, 3.0f, -3.0f, 0.5f, -0.5f, 0.25f, 1.25f, -1.25f, 8.0f, 0.0f, -0.0f };
  int sc = t % 3;
  uint32_t p1[4], p2[4], ad[4];
  int madd_c = is_m(d, "vmaddcfp128"), nms = is_m(d, "vnmsubfp128");
  for (int i = 0; i < 4; i++) {
    p1[i] = xe_pick32(r, xe_fp32, XE_N(xe_fp32));
    p2[i] = xe_pick32(r, xe_fp32, XE_N(xe_fp32));
    ad[i] = xe_pick32(r, xe_fp32, XE_N(xe_fp32));
    if (sc == 0) {
      uint32_t *slot[3] = { &p1[i], &p2[i], &ad[i] };
      uint32_t a = nans[xts_rng_below(r, XE_N(nans))], b = nans[xts_rng_below(r, XE_N(nans))];
      while (b == a) b = nans[xts_rng_below(r, XE_N(nans))];
      int s0 = (int)xts_rng_below(r, 3), s1 = (s0 + 1 + (int)xts_rng_below(r, 2)) % 3;
      *slot[s0] = a;                                       /* two different NaNs in two operand positions */
      *slot[s1] = b;
      if (xe_pct(r, 30)) *slot[3 - s0 - s1] = nans[xts_rng_below(r, XE_N(nans))];
    } else if (sc == 1) {
      uint32_t e1 = 30 + (uint32_t)xts_rng_below(r, 60), e2 = 100 + (uint32_t)xts_rng_below(r, 27) - e1;
      p1[i] = fw(rbit(r), e1, rm23(r));
      p2[i] = fw(rbit(r), e2, rm23(r));
      if (xe_pct(r, 20)) p1[i] = fw(rbit(r), 0, rm23(r));                  /* denormal input */
      uint32_t m = xts_rng_below(r, 4);
      ad[i] = m == 0 ? 0 : m == 1 ? fw(rbit(r), 0, rm23(r)) : m == 2 ? fw(rbit(r), 1 + (uint32_t)xts_rng_below(r, 3), rm23(r)) : fw(rbit(r), e1 / 2, rm23(r));
    } else {
      float a = small[xts_rng_below(r, XE_N(small))], b = small[xts_rng_below(r, XE_N(small))];
      float p = a * b;
      p1[i] = f2u(a);
      p2[i] = f2u(b);
      ad[i] = f2u(nms ? p : -p);
      if (xe_pct(r, 25)) ad[i] = f2u(u2f(ad[i]) * 1.0f) ^ ((uint32_t)xts_rng_below(r, 2) << 31) * 0;   /* keep exact for most lanes */
    }
  }
  int ivd = opi(d, XTS_F_VD128), iva = opi(d, XTS_F_VA128), ivb = opi(d, XTS_F_VB128);
  int rd = (int)ops[ivd], ra = (int)ops[iva], rb = (int)ops[ivb];
  /* product operands / addend per instruction: madd VA*VB+VD, maddc VA*VD+VB, nmsub -(VA*VB-VD) */
  xts_set_vr(x, ra, xts_vr4(p1[0], p1[1], p1[2], p1[3]));
  if (madd_c) {
    xts_set_vr(x, rd, xts_vr4(p2[0], p2[1], p2[2], p2[3]));
    xts_set_vr(x, rb, xts_vr4(ad[0], ad[1], ad[2], ad[3]));
  } else {
    xts_set_vr(x, rb, xts_vr4(p2[0], p2[1], p2[2], p2[3]));
    xts_set_vr(x, rd, xts_vr4(ad[0], ad[1], ad[2], ad[3]));
  }
  if (sc == 1) { x->in.vscr.w[3] |= XTS_VSCR_NJ; *note = "targeted fma: denormal results, NJ forced"; }
  else if (sc == 0) *note = "targeted fma: different NaNs";
  else *note = "targeted fma: exact cancellation";
}

/* ---- vmsum ---- */
static void vmsum_targeted(const xts_insn_t *d, xts_rng_t *r, xts_exec_t *x, const int64_t *ops, int t, const char **note)
{
  static const int diffs[] = { 20, 24, 25, 26, 27, 30, 40, 47, 48, 49, 50, 60, 70, 90, 100, 120 };
  int sc = t % 6, sub = t / 6;
  uint32_t a[4], b[4];
  for (int i = 0; i < 4; i++) { a[i] = 0; b[i] = fw(0, 127, 0); }
  switch (sc) {
  case 0: {
    a[0] = fw(rbit(r), 137, rm23(r)); b[0] = fw(0, 137, rm23(r));            /* p0 ~ 2^20 */
    int df = diffs[sub % XE_N(diffs)];
    a[1] = fw(rbit(r), 127, rm23(r)); b[1] = fw(0, (uint32_t)(147 - df), rm23(r));
    if (sub & 1) { int d2 = diffs[(sub + 5) % XE_N(diffs)]; a[2] = fw(rbit(r), 127, rm23(r)); b[2] = fw(0, (uint32_t)(147 - d2), rm23(r)); }
    if (sub & 2) { int d3 = diffs[(sub + 9) % XE_N(diffs)]; a[3] = fw(rbit(r), 127, rm23(r)); b[3] = fw(0, (uint32_t)(147 - d3), rm23(r)); }
    if (sub & 4) { a[0] ^= 0x80000000u; }                                      /* p0 sign flipped relative to the rest */
    *note = "targeted vmsum: small products below the largest";
    break;
  }
  case 1: {
    uint32_t sx = rbit(r), ex = 100 + (uint32_t)xts_rng_below(r, 50), ey = 100 + (uint32_t)xts_rng_below(r, 50);
    uint32_t xv = fw(sx, ex, rm23(r)), yv = fw(0, ey, rm23(r));
    a[0] = xv; b[0] = yv; a[1] = xv ^ 0x80000000u; b[1] = yv;
    if (sub & 1) { uint32_t zv = fw(rbit(r), 120, rm23(r)); a[2] = zv; b[2] = yv; a[3] = zv ^ 0x80000000u; b[3] = yv; }
    if (sub & 2) { uint32_t tmp = a[0]; a[0] = a[1]; a[1] = tmp; }
    *note = "targeted vmsum: x + (-x)";
    break;
  }
  case 2:
    for (int i = 0; i < 4; i++) { a[i] = fw(rbit(r), 0, 0); b[i] = fw(rbit(r), 120 + (uint32_t)xts_rng_below(r, 15), rm23(r)); }
    if (sub & 1) a[1 + (sub & 2)] = fw(rbit(r), 1, 0) ;              /* a tiny non-zero product among zeros */
    *note = "targeted vmsum: all-zero products, signs";
    break;
  case 3:
    switch (sub % 4) {
    case 0: a[0] = fw(0, 200, 0); b[0] = fw(0, 200, 0); a[1] = fw(1, 200, 0); b[1] = fw(0, 200, 0); break;
    case 1: a[0] = fw(0, 200, 0); b[0] = fw(0, 200, 0); a[1] = fw(1, 200, 0x7FFFFF); b[1] = fw(0, 200, 0); break;
    case 2: a[0] = fw(0, 200, rm23(r)); b[0] = fw(0, 200, rm23(r)); a[1] = fw(rbit(r), 130, rm23(r)); b[1] = fw(0, 128, rm23(r)); break;
    default: a[0] = fw(0, 200, 0); b[0] = fw(0, 200, 0); a[1] = fw(1, 210, rm23(r)); b[1] = fw(0, 210, rm23(r)); break;
    }
    if (sub & 4) { a[2] = fw(rbit(r), 140, rm23(r)); b[2] = fw(0, 120, rm23(r)); }
    *note = "targeted vmsum: product overflow";
    break;
  case 4:
    a[0] = (sub & 1) ? 0xFF800000u : 0x7F800000u; b[0] = fw(0, 127, 0);
    a[1] = fw((sub >> 1) & 1, 200, 0); b[1] = fw(0, 200, 0);
    if (sub & 4) { a[2] = fw(rbit(r), 130, rm23(r)); b[2] = fw(0, 130, rm23(r)); }
    *note = "targeted vmsum: infinity plus overflowing sum";
    break;
  default:
    for (int i = 0; i < 4; i++) {
      a[i] = xe_pct(r, 60) ? fw(rbit(r), 0, rm23(r)) : fw(rbit(r), 100 + (uint32_t)xts_rng_below(r, 40), rm23(r));
      b[i] = xe_pct(r, 40) ? fw(rbit(r), 0, rm23(r)) : fw(rbit(r), 100 + (uint32_t)xts_rng_below(r, 40), rm23(r));
    }
    a[0] = fw(0, 137, rm23(r)); b[0] = fw(0, 137, rm23(r));
    x->in.vscr.w[3] |= XTS_VSCR_NJ;
    *note = "targeted vmsum: denormal inputs, NJ forced";
    break;
  }
  xts_set_vr(x, (int)ops[opi(d, XTS_F_VA128)], xts_vr4(a[0], a[1], a[2], a[3]));
  xts_set_vr(x, (int)ops[opi(d, XTS_F_VB128)], xts_vr4(b[0], b[1], b[2], b[3]));
}

/* ---- vpkd3d128 / vupkd3d128 ---- */
static uint32_t pk_lane(xts_rng_t *r)
{
  static const uint32_t nans[] = { 0x7FC00000, 0x7F800001, 0xFFC00000, 0xFF800001, 0x7FFFFFFF };
  static const int win[] = { 8, 9, 10, 15, 19, 20, 22 };
  uint32_t m = xts_rng_below(r, 100);
  if (m < 28) {                                   /* D3D integer code: 3.0 + n * 2^-22, n around the format windows */
    int b = win[xts_rng_below(r, XE_N(win))];
    int64_t code = (int64_t)xts_rng_range(r, -(1ll << b) - 2, (1ll << b) + 2);
    return (uint32_t)(0x40400000 + code);
  }
  if (m < 36) return 0x40400000u + (uint32_t)(xts_rng_next(r) & 0x3FFFFFF);   /* far outside any window */
  if (m < 48) return nans[xts_rng_below(r, XE_N(nans))];
  if (m < 60) return fw(rbit(r), 102 + (uint32_t)xts_rng_below(r, 11), rm23(r));            /* half denormals / below */
  if (m < 78) {                                   /* half-range exponents, rounding-sensitive mantissa tails */
    static const uint32_t tail[] = { 0x0000, 0x1000, 0x0FFF, 0x1001, 0x1FFF, 0x0800, 0x17FF, 0x1800 };
    return fw(rbit(r), 113 + (uint32_t)xts_rng_below(r, 30), (rm23(r) & ~0x1FFFu) | tail[xts_rng_below(r, XE_N(tail))]);
  }
  if (m < 90) {                                   /* colour range */
    static const uint32_t col[] = { 0x00000000, 0x3F800000, 0x3F000000, 0x3B808081, 0x3F7FFFFF, 0xBF800000, 0x40000000, 0x80000000, 0x3C000000 };
    return col[xts_rng_below(r, XE_N(col))];
  }
  return xe_pick32(r, xe_fp32, XE_N(xe_fp32));
}

static uint32_t upk_word(xts_rng_t *r)
{
  static const uint16_t h[] = { 0x8000, 0x0000, 0x7C00, 0xFC00, 0x0001, 0x03FF, 0x83FF, 0x7FFF, 0xFFFF, 0x3C00, 0xBC00, 0x0400, 0x7BFF, 0x0200, 0x4000 };
  static const uint32_t f10[] = { 0x200, 0x1FF, 0x3FF, 0x000, 0x001, 0x201, 0x1FE, 0x2FF };
  uint32_t m = xts_rng_below(r, 100);
  if (m < 40) return ((uint32_t)h[xts_rng_below(r, XE_N(h))] << 16) | (xe_pct(r, 80) ? h[xts_rng_below(r, XE_N(h))] : (uint32_t)(xts_rng_next(r) & 0xFFFF));
  if (m < 65) {
    uint32_t w = 0;
    for (int i = 0; i < 3; i++) w |= (xe_pct(r, 70) ? f10[xts_rng_below(r, XE_N(f10))] : (uint32_t)(xts_rng_next(r) & 0x3FF)) << (10 * i);
    return w | ((uint32_t)xts_rng_below(r, 4) << 30);
  }
  if (m < 80) {                                   /* 4_20_20_20 style fields */
    static const uint32_t f20[] = { 0x80000, 0x7FFFF, 0xFFFFF, 0, 1, 0x80001, 0x40000 };
    return (f20[xts_rng_below(r, XE_N(f20))] ^ (uint32_t)(xts_rng_below(r, 3) == 0 ? xts_rng_next(r) & 0xFFFFF : 0)) | ((uint32_t)xts_rng_below(r, 16) << 28);
  }
  if (m < 90) { uint32_t b = (uint32_t)xts_rng_next(r); return (b & 0x01010101u) * 0xFF ^ (xe_pct(r, 50) ? 0 : b); }
  return (uint32_t)xts_rng_next(r);
}

static void v128_hook(const xts_insn_t *d, xts_rng_t *r, xts_exec_t *x, const int64_t *ops, int t, const char **note)
{
  if (is_m(d, "vmaddfp128") || is_m(d, "vmaddcfp128") || is_m(d, "vnmsubfp128")) {
    if (t >= 0) fma_targeted(d, r, x, ops, t, note);
  } else if (is_m(d, "vmsum3fp128") || is_m(d, "vmsum4fp128")) {
    if (t >= 0) vmsum_targeted(d, r, x, ops, t, note);
  } else if (is_m(d, "vpkd3d128")) {
    xts_set_vr(x, (int)ops[opi(d, XTS_F_VB128)], xts_vr4(pk_lane(r), pk_lane(r), pk_lane(r), pk_lane(r)));
  } else if (is_m(d, "vupkd3d128")) {
    xts_set_vr(x, (int)ops[opi(d, XTS_F_VB128)], xts_vr4(upk_word(r), upk_word(r), upk_word(r), upk_word(r)));
  }
}

static int v128_run(const xts_case_t *c, xts_ctx_t *ctx) { return xe_vec_run_h(c, ctx, v128_hook); }

XTS_FAMILY(exec_vmx128, .kind = "semantic", .axes = "sf,vscr_nj", .enumerate = v128_enumerate, .run = v128_run,
           .block_cases = 48, .desc = "VMX128 instructions vs the reference model (full immediate ranges, v0..v127, targeted FMA / vmsum / d3d)");
