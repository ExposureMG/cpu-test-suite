/* VMX128-only model (owner: mvmx): the tied-VD fused multiply-adds, the non-IEEE dot products vmsum3fp128/vmsum4fp128,
 * vpermwi128, vrlimi128 and the D3D pack/unpack pair vpkd3d128/vupkd3d128.
 *
 * Sources, in order of trust: results4 (24 cases each, all matched), fmastat results17 (FMA operand order, 256 lanes
 * per op), xenia-canary ppc_emit_altivec.cc + x64_seq_vector.cc (D3D formats and placements). What results4 does not
 * cover is listed in src/model/KNOWN.md, section "vmx128 (mvmx)". Every helper here is file-local: no dependency on
 * the other FP model files.
 *
 * Vector conventions: w[0] = element 0 = x (most significant word). VMX128 vector ops never touch FPSCR; they always
 * round to nearest even and honour VSCR[NJ]. */
#include <xts/model.h>
#include "softfloat.h"

/* ---- VMX128 register fields (fixed positions, see isa/xenon_isa.json "fields") ---- */
static int x_vd(uint32_t w) { return (int)(XM_BITS(w, 21, 5) | XM_BITS(w, 2, 2) << 5); }
static int x_va(uint32_t w) { return (int)(XM_BITS(w, 16, 5) | XM_BITS(w, 5, 1) << 5 | XM_BITS(w, 10, 1) << 6); }
static int x_vb(uint32_t w) { return (int)(XM_BITS(w, 11, 5) | XM_BITS(w, 0, 2) << 5); }

/* ---- single-precision helpers ---- */
#define F_SIGN 0x80000000u
#define F_ABS  0x7FFFFFFFu
#define F_EXP  0x7F800000u
#define F_FRAC 0x007FFFFFu
#define F_QBIT 0x00400000u
#define F_DNAN 0x7FC00000u
static int f_nan(uint32_t b) { return (b & F_ABS) > F_EXP; }
static int f_inf(uint32_t b) { return (b & F_ABS) == F_EXP; }
static int f_zero(uint32_t b) { return (b & F_ABS) == 0; }
static int f_den(uint32_t b) { return (b & F_EXP) == 0 && (b & F_FRAC) != 0; }
static uint32_t f_nj(uint32_t b, int nj) { return nj && f_den(b) ? (b & F_SIGN) : b; }   /* VSCR[NJ] flush */
static int vscr_nj(const xts_state_t *s) { return (s->vscr.w[3] & XTS_VSCR_NJ) != 0; }

/* ======================================================================================================== FMA
 * One rounding (round to nearest even), hardware-decided operand orders (fmastat results17: 256/256 lanes each):
 *   vmaddfp128  VD = VA*VB + VD      vmaddcfp128 VD = VA*VD + VB      vnmsubfp128 VD = -(VA*VB - VD)
 * NaN: the first NaN in the order (first multiplicand, addend, second multiplicand), quieted (OR 0x00400000), never
 * negated; invalid (inf*0, inf-inf) gives 0x7FC00000. NJ=1: denormal inputs and denormal results become signed zeros.
 * vnmsubfp128 negates after rounding, so an exact cancellation gives -0 (SHORTCOMINGS 8.6, results4). */
static uint32_t fma1(uint32_t m1, uint32_t m2, uint32_t add, int negate, int nj)
{
  if (f_nan(m1)) return m1 | F_QBIT;
  if (f_nan(add)) return add | F_QBIT;
  if (f_nan(m2)) return m2 | F_QBIT;
  m1 = f_nj(m1, nj);
  m2 = f_nj(m2, nj);
  add = f_nj(add, nj);
  softfloat_roundingMode = softfloat_round_near_even;
  softfloat_exceptionFlags = 0;
  float32_t a = { m1 }, b = { m2 }, c = { negate ? add ^ F_SIGN : add };
  uint32_t r = f32_mulAdd(a, b, c).v;
  if (f_nan(r)) return F_DNAN;                    /* only invalid operations get here: inputs are not NaN */
  r = f_nj(r, nj);
  return negate ? r ^ F_SIGN : r;
}

enum { FMA_ABD, FMA_ADB, FMA_NABD };
static xts_mres_t vfma128(int kind, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  const xts_vr_t *a = &in->vr[x_va(w)], *b = &in->vr[x_vb(w)], *d = &in->vr[x_vd(w)];
  int nj = vscr_nj(in);
  xts_vr_t r;
  for (int i = 0; i < 4; i++) {
    if (kind == FMA_ABD) r.w[i] = fma1(a->w[i], b->w[i], d->w[i], 0, nj);
    else if (kind == FMA_ADB) r.w[i] = fma1(a->w[i], d->w[i], b->w[i], 0, nj);
    else r.w[i] = fma1(a->w[i], b->w[i], d->w[i], 1, nj);
  }
  out->vr[x_vd(w)] = r;
  return XTS_M_OK;
}

static xts_mres_t m_vmaddfp128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  return vfma128(FMA_ABD, w, in, out);
}
XTS_MODEL("vmaddfp128", m_vmaddfp128);

static xts_mres_t m_vmaddcfp128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  return vfma128(FMA_ADB, w, in, out);
}
XTS_MODEL("vmaddcfp128", m_vmaddcfp128);

static xts_mres_t m_vnmsubfp128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  return vfma128(FMA_NABD, w, in, out);
}
XTS_MODEL("vnmsubfp128", m_vnmsubfp128);

/* ================================================================================================ dot products
 * vmsum3fp128 (lanes x,y,z; w is ignored) and vmsum4fp128 (x,y,z,w); the scalar result is splatted to all four words.
 * Not an IEEE sum. The formulation below matches all 48 results4 cases (24 + 24); it was picked by a search over
 * summation order x product rounding x accumulator width x final rounding x sign handling (KNOWN.md):
 *   1. NJ=1 flushes denormal inputs to signed zeros (NJ=0 keeps them: inf*denormal = inf in results4).
 *   2. Any NaN input -> 0x7FC00000 (default NaN, an SNaN payload is not kept). inf*0 or inf-inf -> 0x7FC00000;
 *      otherwise an infinite product gives that infinity.
 *   3. Exact 48-bit products p_i = m_i * 2^e_i, aligned (truncated) to the LSB of the largest exponent sum.
 *   4. One's-complement accumulation relative to the sign of p0 WITHOUT the end-around carry: products with p0's
 *      sign add their aligned magnitude, the others add ~magnitude (= -magnitude - 1). A negative total flips the
 *      sign and is complemented back (~total). So every opposite-signed product (even a zero) costs one LSB of the
 *      accumulator when the total keeps p0's sign: -1.5 + -0.75 + (+0) = 0xC00FFFFF.
 *   5. Truncation (toward zero) to 24 bits. Exponent overflow -> QNaN with the result's sign (0x7FC00000 or
 *      0xFFC00000), never infinity. A result below 2^-126 -> signed zero, whatever NJ says.
 * Free parameters the data does not fix: the accumulator may keep 0..40 more bits below the aligned LSB (VMSUM_GUARD,
 * all give 48/48), and the products may be rounded to 24 bits first (also 48/48). */
#define VMSUM_GUARD 0
_Static_assert(VMSUM_GUARD >= 0 && VMSUM_GUARD <= 12, "48-bit products << VMSUM_GUARD must fit the 64-bit accumulator");
static uint32_t vmsum(const xts_vr_t *va, const xts_vr_t *vb, int n, int nj)
{
  uint32_t a[4], b[4];
  int nan = 0, inf_pos = 0, inf_neg = 0, invalid = 0;
  for (int i = 0; i < n; i++) {
    a[i] = f_nj(va->w[i], nj);
    b[i] = f_nj(vb->w[i], nj);
    if (f_nan(a[i]) || f_nan(b[i])) nan = 1;
  }
  if (nan) return F_DNAN;
  for (int i = 0; i < n; i++) {
    if (!f_inf(a[i]) && !f_inf(b[i])) continue;
    if (f_zero(a[i]) || f_zero(b[i])) invalid = 1;
    else if ((a[i] ^ b[i]) & F_SIGN) inf_neg = 1;
    else inf_pos = 1;
  }
  if (invalid || (inf_pos && inf_neg)) return F_DNAN;
  if (inf_pos) return F_EXP;
  if (inf_neg) return F_SIGN | F_EXP;
  /* products: m (< 2^48) * 2^e */
  uint64_t pm[4] = { 0 };
  int pe[4] = { 0 }, ps[4] = { 0 }, emax = -100000;
  for (int i = 0; i < n; i++) {
    uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
    uint64_t ma = (a[i] & F_FRAC) | (ea ? 0x800000u : 0), mb = (b[i] & F_FRAC) | (eb ? 0x800000u : 0);
    pm[i] = ma * mb;
    pe[i] = (int)(ea ? ea : 1) + (int)(eb ? eb : 1) - 254 - 46;
    ps[i] = (int)((a[i] ^ b[i]) >> 31);
    if (pe[i] > emax) emax = pe[i];
  }
  int lsb = emax - VMSUM_GUARD;
  int64_t acc = 0;
  for (int i = 0; i < n; i++) {
    int sh = lsb - pe[i];
    uint64_t v = sh >= 64 ? 0 : (sh >= 0 ? pm[i] >> sh : pm[i] << -sh);
    acc += ps[i] == ps[0] ? (int64_t)v : -(int64_t)v - 1;
  }
  uint32_t sign = (uint32_t)ps[0] << 31;
  uint64_t mag;
  if (acc < 0) { sign ^= F_SIGN; mag = (uint64_t)(-(acc + 1)); }
  else mag = (uint64_t)acc;
  if (!mag) return sign;
  int nb = 64 - __builtin_clzll(mag);
  int e = lsb + nb - 1;                           /* unbiased exponent of the MSB */
  uint32_t frac = (uint32_t)(nb > 24 ? mag >> (nb - 24) : mag << (24 - nb)) & F_FRAC;   /* truncate */
  if (e > 127) return sign | F_DNAN;
  if (e < -126) return sign;
  return sign | (uint32_t)(e + 127) << 23 | frac;
}

static xts_mres_t vmsum128(int n, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  uint32_t r = vmsum(&in->vr[x_va(w)], &in->vr[x_vb(w)], n, vscr_nj(in));
  out->vr[x_vd(w)] = xts_vr4(r, r, r, r);
  return XTS_M_OK;
}

static xts_mres_t m_vmsum3fp128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  return vmsum128(3, w, in, out);
}
XTS_MODEL("vmsum3fp128", m_vmsum3fp128);

static xts_mres_t m_vmsum4fp128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  return vmsum128(4, w, in, out);
}
XTS_MODEL("vmsum4fp128", m_vmsum4fp128);

/* ================================================================================================ permutes
 * vpermwi128 VD,VB,PERM: VD.x,y,z,w = VB[PERM>>6&3], VB[PERM>>4&3], VB[PERM>>2&3], VB[PERM&3]
 * (PERM = bits 16-20 | bits 6-8 << 5). 24/24 results4 cases. */
static xts_mres_t m_vpermwi128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  uint32_t perm = XM_BITS(w, 16, 5) | XM_BITS(w, 6, 3) << 5;
  const xts_vr_t *b = &in->vr[x_vb(w)];
  xts_vr_t r;
  for (int i = 0; i < 4; i++) r.w[i] = b->w[(perm >> (6 - 2 * i)) & 3];
  out->vr[x_vd(w)] = r;
  return XTS_M_OK;
}
XTS_MODEL("vpermwi128", m_vpermwi128);

/* vrlimi128 VD,VB,IMM,z: rotate VB left by z words, then insert word i where IMM bit (3-i) is set (bit 3 = x).
 * 24/24 results4 cases (IMM 0-15). IMM bit 4 (16-31) was never executed; modelled as ignored. */
static xts_mres_t m_vrlimi128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  uint32_t imm = XM_BITS(w, 16, 5), z = XM_BITS(w, 6, 2);
  const xts_vr_t *b = &in->vr[x_vb(w)];
  xts_vr_t r = in->vr[x_vd(w)];
  for (int i = 0; i < 4; i++)
    if ((imm >> (3 - i)) & 1) r.w[i] = b->w[(i + (int)z) & 3];
  out->vr[x_vd(w)] = r;
  return XTS_M_OK;
}
XTS_MODEL("vrlimi128", m_vrlimi128);

/* ================================================================================================ D3D formats
 * The integer formats work on floats biased by 3.0 (0x40400000): the integer is (bits - 0x40400000), i.e. the value
 * (v - 3.0) * 2^22, exact in the window. Pack clamps to the format range and sets VSCR[SAT] when it clamps; a NaN
 * packs as the most negative code (0x8000 / 0x200 / 0x80000, the code unpack turns back into a QNaN) without SAT.
 * Unsigned fields (D3DCOLOR, the 2/4-bit W) clamp at 0. The FLOAT16 formats are the Xenos half (no inf/NaN:
 * |v| >= 0x47FFE000 and NaN saturate to 0x7FFF | sign, |v| < 2^-14 gives a signed zero) and never set SAT.
 * Packed layouts (P = the packed vector before placement, unused words 0):
 *   0 D3DCOLOR      P.w = W<<24 | X<<16 | Y<<8 | Z            (each 0..255)
 *   1 NORMSHORT2    P.w = X<<16 | Y                           (-32767..32767)
 *   2 2_10_10_10    P.w = W<<30 | Z<<20 | Y<<10 | X           (XYZ -511..511, W 0..3)
 *   3 FLOAT16_2     P.w = h(X)<<16 | h(Y)                     (truncating, as xenia)
 *   4 NORMSHORT4    P.z = X<<16 | Y, P.w = Z<<16 | W
 *   5 FLOAT16_4     P.z = h(X)<<16 | h(Y), P.w = h(Z)<<16 | h(W)   (round to nearest even, as xenia)
 *   6 4_20_20_20    P.z:P.w = W<<60 | Z<<40 | Y<<20 | X       (XYZ -(2^19-1)..2^19-1, W 0..15)
 * Types 0-5 are confirmed by results4 (saturating and NaN lanes only); type 6 and the float16 rounding of in-range
 * values are xenia's. Type 7 and pack size 0 are not modelled (XTS_M_UNIMPLEMENTED). */
static uint32_t d3d_int(uint32_t b, int32_t lo, int32_t hi, int32_t nanv, int bits, int *sat)
{
  int32_t v;
  if (f_nan(b)) v = nanv;
  else if (b & F_SIGN) { v = lo; *sat = 1; }
  else {
    int64_t k = (int64_t)b - 0x40400000;
    if (k < lo) { v = lo; *sat = 1; }
    else if (k > hi) { v = hi; *sat = 1; }
    else v = (int32_t)k;
  }
  return (uint32_t)v & ((1u << bits) - 1);
}

static uint32_t xenos_half(uint32_t b, int rne)
{
  uint32_t a = b & F_ABS, r;
  if (a >= 0x47FFE000u) r = 0x7FFF;               /* also inf and NaN */
  else {
    r = a < 0x38800000u ? 0 : a + 0xC8000000u;
    if (rne) r += 0xFFF + ((r >> 13) & 1);
    r = (r >> 13) & 0x7FFF;
  }
  return r | ((b >> 16) & 0x8000);
}

/* returns 0 for an unknown type */
static int d3d_pack(int type, const xts_vr_t *v, uint32_t p[4], int *sat)
{
  const uint32_t *x = v->w;
  p[0] = p[1] = p[2] = p[3] = 0;
  switch (type) {
  case 0: {
    uint32_t c[4];
    for (int i = 0; i < 4; i++) c[i] = d3d_int(x[i], 0, 255, 0, 8, sat);
    p[3] = c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
    return 1;
  }
  case 1:
    p[3] = d3d_int(x[0], -32767, 32767, -32768, 16, sat) << 16 | d3d_int(x[1], -32767, 32767, -32768, 16, sat);
    return 1;
  case 2:
    p[3] = d3d_int(x[3], 0, 3, 0, 2, sat) << 30 | d3d_int(x[2], -511, 511, -512, 10, sat) << 20 |
           d3d_int(x[1], -511, 511, -512, 10, sat) << 10 | d3d_int(x[0], -511, 511, -512, 10, sat);
    return 1;
  case 3:
    p[3] = xenos_half(x[0], 0) << 16 | xenos_half(x[1], 0);
    return 1;
  case 4:
    p[2] = d3d_int(x[0], -32767, 32767, -32768, 16, sat) << 16 | d3d_int(x[1], -32767, 32767, -32768, 16, sat);
    p[3] = d3d_int(x[2], -32767, 32767, -32768, 16, sat) << 16 | d3d_int(x[3], -32767, 32767, -32768, 16, sat);
    return 1;
  case 5:
    p[2] = xenos_half(x[0], 1) << 16 | xenos_half(x[1], 1);
    p[3] = xenos_half(x[2], 1) << 16 | xenos_half(x[3], 1);
    return 1;
  case 6: {
    const int32_t m = (1 << 19) - 1;
    uint64_t X = d3d_int(x[0], -m, m, -m - 1, 20, sat), Y = d3d_int(x[1], -m, m, -m - 1, 20, sat);
    uint64_t Z = d3d_int(x[2], -m, m, -m - 1, 20, sat), W = d3d_int(x[3], 0, 15, 0, 4, sat);
    uint64_t q = W << 60 | Z << 40 | Y << 20 | X;
    p[2] = (uint32_t)(q >> 32);
    p[3] = (uint32_t)q;
    return 1;
  }
  }
  return 0;
}

/* vpkd3d128 VD,VB,type,pack,z: the IMM field (bits 16-20) is type<<2 | pack, z = bits 6-7.
 * Placement (xenia, every combination results4 ran agrees): which words of VD receive which words of P. */
static const int8_t PK_PLACE[4][4][2][2] = {      /* [pack][z][k] = {dest word, P word}, -1 = none */
  { { { -1, -1 }, { -1, -1 } }, { { -1, -1 }, { -1, -1 } }, { { -1, -1 }, { -1, -1 } }, { { -1, -1 }, { -1, -1 } } },
  { { { 3, 3 }, { -1, -1 } }, { { 2, 3 }, { -1, -1 } }, { { 1, 3 }, { -1, -1 } }, { { 0, 3 }, { -1, -1 } } },
  { { { 2, 2 }, { 3, 3 } }, { { 1, 2 }, { 2, 3 } }, { { 0, 2 }, { 1, 3 } }, { { 0, 3 }, { -1, -1 } } },
  { { { 2, 2 }, { 3, 3 } }, { { 1, 2 }, { 2, 3 } }, { { 0, 2 }, { 1, 3 } }, { { 3, 2 }, { -1, -1 } } },
};
static xts_mres_t m_vpkd3d128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  uint32_t imm = XM_BITS(w, 16, 5), z = XM_BITS(w, 6, 2);
  int type = (int)(imm >> 2), pack = (int)(imm & 3), sat = 0;
  uint32_t p[4];
  if (pack == 0 || !d3d_pack(type, &in->vr[x_vb(w)], p, &sat)) return XTS_M_UNIMPLEMENTED;
  xts_vr_t r = in->vr[x_vd(w)];
  for (int k = 0; k < 2; k++)
    if (PK_PLACE[pack][z][k][0] >= 0) r.w[PK_PLACE[pack][z][k][0]] = p[PK_PLACE[pack][z][k][1]];
  out->vr[x_vd(w)] = r;
  if (sat) out->vscr.w[3] |= XTS_VSCR_SAT;
  return XTS_M_OK;
}
XTS_MODEL("vpkd3d128", m_vpkd3d128);

/* vupkd3d128 VD,VB,UIMM: type = UIMM>>2 (the low two bits are ignored: UIMM 0-3 and 4-7 agree in results4).
 * Integer codes become 3.0 + code*2^-22 (bits 0x40400000 + sign-extended code), the most negative code becomes
 * 0x7FC00000; unsigned fields: D3DCOLOR channels 1.0 + c*2^-23 (0x3F800000 | c), W of 2_10_10_10 / 4_20_20_20 is
 * 0x3F800000 + w. FLOAT16 uses the Xenos half without denormals (exponent 0 -> signed zero, exponent 31 is finite).
 *   0 D3DCOLOR   from VB.w = ARGB: x=R y=G z=B w=A          1 NORMSHORT2  x,y from VB.w hi/lo; z = 0, w = 1.0
 *   2 2_10_10_10 from VB.w                                   3 FLOAT16_2   x,y from VB.w hi/lo; z = 0, w = 1.0
 *   4 NORMSHORT4 x,y from VB.z, z,w from VB.w               5 FLOAT16_4   same layout
 *   6 4_20_20_20 from VB.z:VB.w
 * Types 0 and 1 are confirmed by results4 (UIMM 0-7), 2-6 are xenia's; type 7 is not modelled. */
static uint32_t upk_int(uint32_t code, int bits)
{
  uint32_t c = code & ((1u << bits) - 1);
  if (c == 1u << (bits - 1)) return F_DNAN;
  int32_t s = (c >> (bits - 1)) ? (int32_t)c - (int32_t)(1u << bits) : (int32_t)c;
  return (uint32_t)(0x40400000 + s);
}

static uint32_t xenos_half_to_f32(uint32_t h)
{
  uint32_t e = (h >> 10) & 0x1F, s = (h & 0x8000u) << 16;
  if (!e) return s;
  return s | (e + 112) << 23 | (h & 0x3FF) << 13;
}

static xts_mres_t m_vupkd3d128(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  int type = (int)(XM_BITS(w, 16, 5) >> 2);
  const xts_vr_t *b = &in->vr[x_vb(w)];
  uint32_t bz = b->w[2], bw = b->w[3];
  xts_vr_t r;
  switch (type) {
  case 0:
    r = xts_vr4(0x3F800000u | ((bw >> 16) & 0xFF), 0x3F800000u | ((bw >> 8) & 0xFF), 0x3F800000u | (bw & 0xFF),
                0x3F800000u | (bw >> 24));
    break;
  case 1: r = xts_vr4(upk_int(bw >> 16, 16), upk_int(bw, 16), 0, 0x3F800000u); break;
  case 2: r = xts_vr4(upk_int(bw, 10), upk_int(bw >> 10, 10), upk_int(bw >> 20, 10), 0x3F800000u + (bw >> 30)); break;
  case 3: r = xts_vr4(xenos_half_to_f32(bw >> 16), xenos_half_to_f32(bw & 0xFFFF), 0, 0x3F800000u); break;
  case 4: r = xts_vr4(upk_int(bz >> 16, 16), upk_int(bz, 16), upk_int(bw >> 16, 16), upk_int(bw, 16)); break;
  case 5:
    r = xts_vr4(xenos_half_to_f32(bz >> 16), xenos_half_to_f32(bz & 0xFFFF), xenos_half_to_f32(bw >> 16),
                xenos_half_to_f32(bw & 0xFFFF));
    break;
  case 6: {
    uint64_t q = (uint64_t)bz << 32 | bw;
    r = xts_vr4(upk_int((uint32_t)q, 20), upk_int((uint32_t)(q >> 20), 20), upk_int((uint32_t)(q >> 40), 20),
                0x3F800000u + (uint32_t)((q >> 60) & 0xF));
    break;
  }
  default: return XTS_M_UNIMPLEMENTED;
  }
  out->vr[x_vd(w)] = r;
  return XTS_M_OK;
}
XTS_MODEL("vupkd3d128", m_vupkd3d128);
