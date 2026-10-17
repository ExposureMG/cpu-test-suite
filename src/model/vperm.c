/* Vector integer model, part 2: data movement - merge, pack/unpack (incl. pixel), permute, select, octet shifts,
 * whole-register bit shifts. AltiVec and VMX128 forms. Saturating packs set VSCR[SAT]. See vint.c for conventions. */
#include "vint_priv.h"

static void to_bytes(const xts_vr_t *v, uint8_t *b) { for (int i = 0; i < 16; i++) b[i] = xts_vr_u8(v, i); }
static xts_vr_t from_bytes(const uint8_t *b) { return xts_vr_load(b); }

/* vmrgh / vmrgl: interleave the high (lo=0) or low (lo=1) half of the elements of vA and vB */
static xts_mres_t vmerge(int esz, int lo, int x128, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  const xts_vr_t *a = &in->vr[vi_va(w, x128)], *b = &in->vr[vi_vb(w, x128)];
  int n = 16 / esz, h = n / 2;
  xts_vr_t r = { { 0, 0, 0, 0 } };
  for (int i = 0; i < h; i++) {
    vi_set(&r, esz, 2 * i, vi_get(a, esz, i + lo * h));
    vi_set(&r, esz, 2 * i + 1, vi_get(b, esz, i + lo * h));
  }
  out->vr[vi_vd(w, x128)] = r;
  return XTS_M_OK;
}

/* pack: source elements of ssz bytes (2 or 4) from vA||vB, destination elements of ssz/2 bytes.
 * mode 0 modulo, 1 unsigned->unsigned saturate, 2 signed->unsigned saturate, 3 signed->signed saturate */
static xts_mres_t vpack(int ssz, int mode, int x128, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  const xts_vr_t *a = &in->vr[vi_va(w, x128)], *b = &in->vr[vi_vb(w, x128)];
  int dsz = ssz / 2, n = 16 / ssz, sat = 0, dbits = 8 * dsz;
  int64_t umax = ((int64_t)1 << dbits) - 1, smax = ((int64_t)1 << (dbits - 1)) - 1, smin = -smax - 1;
  xts_vr_t r = { { 0, 0, 0, 0 } };
  for (int i = 0; i < 2 * n; i++) {
    uint32_t u = vi_get(i < n ? a : b, ssz, i % n);
    int64_t s = vi_sext(u, ssz), v;
    if (mode == 0) v = u;
    else if (mode == 1) { v = u; if (v > umax) { v = umax; sat = 1; } }
    else if (mode == 2) { v = s; if (v > umax) { v = umax; sat = 1; } else if (v < 0) { v = 0; sat = 1; } }
    else { v = s; if (v > smax) { v = smax; sat = 1; } else if (v < smin) { v = smin; sat = 1; } }
    vi_set(&r, dsz, i, (uint32_t)v);
  }
  out->vr[vi_vd(w, x128)] = r;
  if (sat) vi_set_sat(out);
  return XTS_M_OK;
}

/* vpkpx: 8/8/8/8 pixel words -> 1/5/5/5 halfwords (bit 7 of the word, then the top 5 bits of each of the 3 low bytes) */
static xts_mres_t m_vpkpx(VI_PARAMS)
{
  VI_UNUSED;
  const xts_vr_t *a = &in->vr[XM_RA(w)], *b = &in->vr[XM_RB(w)];
  xts_vr_t r = { { 0, 0, 0, 0 } };
  for (int i = 0; i < 8; i++) {
    uint32_t x = (i < 4 ? a : b)->w[i & 3];
    xts_vr_set_u16(&r, i, (uint16_t)(((x >> 24 & 1) << 15) | ((x >> 19 & 0x1F) << 10) | ((x >> 11 & 0x1F) << 5) | (x >> 3 & 0x1F)));
  }
  out->vr[XM_RD(w)] = r;
  return XTS_M_OK;
}
XTS_MODEL("vpkpx", m_vpkpx);

/* vupkh/vupkl s b/h: sign-extend the high (lo=0) or low (lo=1) half of the elements of vB */
static xts_mres_t vunpack(int ssz, int lo, int x128, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  const xts_vr_t *b = &in->vr[vi_vb(w, x128)];
  int n = 16 / ssz / 2;
  xts_vr_t r = { { 0, 0, 0, 0 } };
  for (int i = 0; i < n; i++) vi_set(&r, ssz * 2, i, (uint32_t)vi_sext(vi_get(b, ssz, i + lo * n), ssz));
  out->vr[vi_vd(w, x128)] = r;
  return XTS_M_OK;
}

/* vupkhpx / vupklpx: 1/5/5/5 -> 8/8/8/8 (the 1-bit field is sign-extended to 8 bits, the 5-bit fields zero-extended) */
static xts_mres_t vunpackpx(int lo, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  const xts_vr_t *b = &in->vr[XM_RB(w)];
  xts_vr_t r = { { 0, 0, 0, 0 } };
  for (int i = 0; i < 4; i++) {
    uint32_t h = xts_vr_u16(b, i + 4 * lo);
    r.w[i] = ((h & 0x8000) ? 0xFF000000u : 0) | ((h >> 10 & 0x1F) << 16) | ((h >> 5 & 0x1F) << 8) | (h & 0x1F);
  }
  out->vr[XM_RD(w)] = r;
  return XTS_M_OK;
}

/* vperm: vD.byte[i] = (vA||vB).byte[vC.byte[i] & 31] ; vperm128 takes vC from v0-v7 (VC128 field) */
static xts_mres_t vperm(int x128, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  int c = x128 ? (int)xts_field_get(XTS_F_VC128, w) : (int)xts_field_get(XTS_F_VC, w);
  uint8_t ab[32], cb[16], r[16];
  to_bytes(&in->vr[vi_va(w, x128)], ab);
  to_bytes(&in->vr[vi_vb(w, x128)], ab + 16);
  to_bytes(&in->vr[c], cb);
  for (int i = 0; i < 16; i++) r[i] = ab[cb[i] & 31];
  out->vr[vi_vd(w, x128)] = from_bytes(r);
  return XTS_M_OK;
}

/* vsldoi: vD = bytes sh..sh+15 of vA||vB */
static xts_mres_t vsldoi(int x128, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  int sh = (int)xts_field_get(XTS_F_SHB, w);
  uint8_t ab[32], r[16];
  to_bytes(&in->vr[vi_va(w, x128)], ab);
  to_bytes(&in->vr[vi_vb(w, x128)], ab + 16);
  for (int i = 0; i < 16; i++) r[i] = ab[i + sh];
  out->vr[vi_vd(w, x128)] = from_bytes(r);
  return XTS_M_OK;
}

/* vsel: vD = (vA & ~vC) | (vB & vC) */
static xts_mres_t m_vsel(VI_PARAMS)
{
  VI_UNUSED;
  const xts_vr_t *a = &in->vr[XM_RA(w)], *b = &in->vr[XM_RB(w)], *c = &in->vr[xts_field_get(XTS_F_VC, w)];
  xts_vr_t r;
  for (int i = 0; i < 4; i++) r.w[i] = (a->w[i] & ~c->w[i]) | (b->w[i] & c->w[i]);
  out->vr[XM_RD(w)] = r;
  return XTS_M_OK;
}
XTS_MODEL("vsel", m_vsel);

/* vsel128: the mask is the old value of vD (tied operand): vD = (vA & ~vD) | (vB & vD) */
static xts_mres_t m_vsel128(VI_PARAMS)
{
  VI_UNUSED;
  int dd = vi_vd(w, 1);
  const xts_vr_t *a = &in->vr[vi_va(w, 1)], *b = &in->vr[vi_vb(w, 1)], *c = &in->vr[dd];
  xts_vr_t r;
  for (int i = 0; i < 4; i++) r.w[i] = (a->w[i] & ~c->w[i]) | (b->w[i] & c->w[i]);
  out->vr[dd] = r;
  return XTS_M_OK;
}
XTS_MODEL("vsel128", m_vsel128);

/* vslo / vsro: shift the whole register left/right by whole octets; count = vB bits 121:124 (byte 15, bits 3-6) */
static xts_mres_t voctet(int right, int x128, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  uint8_t a[16], b[16], r[16];
  to_bytes(&in->vr[vi_va(w, x128)], a);
  to_bytes(&in->vr[vi_vb(w, x128)], b);
  int n = (b[15] >> 3) & 0xF;
  for (int i = 0; i < 16; i++) {
    int s = right ? i - n : i + n;
    r[i] = (s >= 0 && s < 16) ? a[s] : 0;
  }
  out->vr[vi_vd(w, x128)] = from_bytes(r);
  return XTS_M_OK;
}

/* vsl / vsr. The architecture shifts the whole register by vB bits 125:127 and leaves the result undefined unless all
 * 16 bytes of vB carry the same low 3 bits. Xenon (results4: 41 of 48 cases have differing counts) does it PER BYTE:
 * result byte i uses the count (vB.byte[i] & 7) and takes the bits shifted in from the neighbouring byte of vA
 * (vsl: the next byte, 0 past the end; vsr: the previous byte, 0 before the start). With equal counts this is the
 * architected whole-register shift. */
static xts_mres_t vbits(int right, uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  uint8_t a[16], b[16], r[16];
  to_bytes(&in->vr[XM_RA(w)], a);
  to_bytes(&in->vr[XM_RB(w)], b);
  for (int i = 0; i < 16; i++) {
    int sh = b[i] & 7;
    if (!sh) { r[i] = a[i]; continue; }
    if (!right) r[i] = (uint8_t)((a[i] << sh) | (i < 15 ? a[i + 1] >> (8 - sh) : 0));
    else r[i] = (uint8_t)((a[i] >> sh) | (i > 0 ? a[i - 1] << (8 - sh) : 0));
  }
  out->vr[XM_RD(w)] = from_bytes(r);
  return XTS_M_OK;
}

static xts_mres_t m_vmrghb(VI_PARAMS) { VI_UNUSED; return vmerge(1, 0, 0, w, in, out); }
XTS_MODEL("vmrghb", m_vmrghb);

static xts_mres_t m_vmrglb(VI_PARAMS) { VI_UNUSED; return vmerge(1, 1, 0, w, in, out); }
XTS_MODEL("vmrglb", m_vmrglb);

static xts_mres_t m_vmrghh(VI_PARAMS) { VI_UNUSED; return vmerge(2, 0, 0, w, in, out); }
XTS_MODEL("vmrghh", m_vmrghh);

static xts_mres_t m_vmrglh(VI_PARAMS) { VI_UNUSED; return vmerge(2, 1, 0, w, in, out); }
XTS_MODEL("vmrglh", m_vmrglh);

static xts_mres_t m_vmrghw(VI_PARAMS) { VI_UNUSED; return vmerge(4, 0, 0, w, in, out); }
XTS_MODEL("vmrghw", m_vmrghw);

static xts_mres_t m_vmrglw(VI_PARAMS) { VI_UNUSED; return vmerge(4, 1, 0, w, in, out); }
XTS_MODEL("vmrglw", m_vmrglw);

static xts_mres_t m_vmrghw128(VI_PARAMS) { VI_UNUSED; return vmerge(4, 0, 1, w, in, out); }
XTS_MODEL("vmrghw128", m_vmrghw128);

static xts_mres_t m_vmrglw128(VI_PARAMS) { VI_UNUSED; return vmerge(4, 1, 1, w, in, out); }
XTS_MODEL("vmrglw128", m_vmrglw128);

static xts_mres_t m_vpkuhum(VI_PARAMS) { VI_UNUSED; return vpack(2, 0, 0, w, in, out); }
XTS_MODEL("vpkuhum", m_vpkuhum);

static xts_mres_t m_vpkuhum128(VI_PARAMS) { VI_UNUSED; return vpack(2, 0, 1, w, in, out); }
XTS_MODEL("vpkuhum128", m_vpkuhum128);

static xts_mres_t m_vpkuwum(VI_PARAMS) { VI_UNUSED; return vpack(4, 0, 0, w, in, out); }
XTS_MODEL("vpkuwum", m_vpkuwum);

static xts_mres_t m_vpkuwum128(VI_PARAMS) { VI_UNUSED; return vpack(4, 0, 1, w, in, out); }
XTS_MODEL("vpkuwum128", m_vpkuwum128);

static xts_mres_t m_vpkuhus(VI_PARAMS) { VI_UNUSED; return vpack(2, 1, 0, w, in, out); }
XTS_MODEL("vpkuhus", m_vpkuhus);

static xts_mres_t m_vpkuhus128(VI_PARAMS) { VI_UNUSED; return vpack(2, 1, 1, w, in, out); }
XTS_MODEL("vpkuhus128", m_vpkuhus128);

static xts_mres_t m_vpkuwus(VI_PARAMS) { VI_UNUSED; return vpack(4, 1, 0, w, in, out); }
XTS_MODEL("vpkuwus", m_vpkuwus);

static xts_mres_t m_vpkuwus128(VI_PARAMS) { VI_UNUSED; return vpack(4, 1, 1, w, in, out); }
XTS_MODEL("vpkuwus128", m_vpkuwus128);

static xts_mres_t m_vpkshus(VI_PARAMS) { VI_UNUSED; return vpack(2, 2, 0, w, in, out); }
XTS_MODEL("vpkshus", m_vpkshus);

static xts_mres_t m_vpkshus128(VI_PARAMS) { VI_UNUSED; return vpack(2, 2, 1, w, in, out); }
XTS_MODEL("vpkshus128", m_vpkshus128);

static xts_mres_t m_vpkswus(VI_PARAMS) { VI_UNUSED; return vpack(4, 2, 0, w, in, out); }
XTS_MODEL("vpkswus", m_vpkswus);

static xts_mres_t m_vpkswus128(VI_PARAMS) { VI_UNUSED; return vpack(4, 2, 1, w, in, out); }
XTS_MODEL("vpkswus128", m_vpkswus128);

static xts_mres_t m_vpkshss(VI_PARAMS) { VI_UNUSED; return vpack(2, 3, 0, w, in, out); }
XTS_MODEL("vpkshss", m_vpkshss);

static xts_mres_t m_vpkshss128(VI_PARAMS) { VI_UNUSED; return vpack(2, 3, 1, w, in, out); }
XTS_MODEL("vpkshss128", m_vpkshss128);

static xts_mres_t m_vpkswss(VI_PARAMS) { VI_UNUSED; return vpack(4, 3, 0, w, in, out); }
XTS_MODEL("vpkswss", m_vpkswss);

static xts_mres_t m_vpkswss128(VI_PARAMS) { VI_UNUSED; return vpack(4, 3, 1, w, in, out); }
XTS_MODEL("vpkswss128", m_vpkswss128);

static xts_mres_t m_vupkhsb(VI_PARAMS) { VI_UNUSED; return vunpack(1, 0, 0, w, in, out); }
XTS_MODEL("vupkhsb", m_vupkhsb);

static xts_mres_t m_vupkhsb128(VI_PARAMS) { VI_UNUSED; return vunpack(1, 0, 1, w, in, out); }
XTS_MODEL("vupkhsb128", m_vupkhsb128);

static xts_mres_t m_vupklsb(VI_PARAMS) { VI_UNUSED; return vunpack(1, 1, 0, w, in, out); }
XTS_MODEL("vupklsb", m_vupklsb);

static xts_mres_t m_vupklsb128(VI_PARAMS) { VI_UNUSED; return vunpack(1, 1, 1, w, in, out); }
XTS_MODEL("vupklsb128", m_vupklsb128);

static xts_mres_t m_vupkhsh(VI_PARAMS) { VI_UNUSED; return vunpack(2, 0, 0, w, in, out); }
XTS_MODEL("vupkhsh", m_vupkhsh);

static xts_mres_t m_vupkhsh128(VI_PARAMS) { VI_UNUSED; return vunpack(2, 0, 1, w, in, out); }
XTS_MODEL("vupkhsh128", m_vupkhsh128);

static xts_mres_t m_vupklsh(VI_PARAMS) { VI_UNUSED; return vunpack(2, 1, 0, w, in, out); }
XTS_MODEL("vupklsh", m_vupklsh);

static xts_mres_t m_vupklsh128(VI_PARAMS) { VI_UNUSED; return vunpack(2, 1, 1, w, in, out); }
XTS_MODEL("vupklsh128", m_vupklsh128);

static xts_mres_t m_vupkhpx(VI_PARAMS) { VI_UNUSED; return vunpackpx(0, w, in, out); }
XTS_MODEL("vupkhpx", m_vupkhpx);

static xts_mres_t m_vupklpx(VI_PARAMS) { VI_UNUSED; return vunpackpx(1, w, in, out); }
XTS_MODEL("vupklpx", m_vupklpx);

static xts_mres_t m_vperm(VI_PARAMS) { VI_UNUSED; return vperm(0, w, in, out); }
XTS_MODEL("vperm", m_vperm);

static xts_mres_t m_vperm128(VI_PARAMS) { VI_UNUSED; return vperm(1, w, in, out); }
XTS_MODEL("vperm128", m_vperm128);

static xts_mres_t m_vsldoi(VI_PARAMS) { VI_UNUSED; return vsldoi(0, w, in, out); }
XTS_MODEL("vsldoi", m_vsldoi);

static xts_mres_t m_vsldoi128(VI_PARAMS) { VI_UNUSED; return vsldoi(1, w, in, out); }
XTS_MODEL("vsldoi128", m_vsldoi128);

static xts_mres_t m_vslo(VI_PARAMS) { VI_UNUSED; return voctet(0, 0, w, in, out); }
XTS_MODEL("vslo", m_vslo);

static xts_mres_t m_vsro(VI_PARAMS) { VI_UNUSED; return voctet(1, 0, w, in, out); }
XTS_MODEL("vsro", m_vsro);

static xts_mres_t m_vslo128(VI_PARAMS) { VI_UNUSED; return voctet(0, 1, w, in, out); }
XTS_MODEL("vslo128", m_vslo128);

static xts_mres_t m_vsro128(VI_PARAMS) { VI_UNUSED; return voctet(1, 1, w, in, out); }
XTS_MODEL("vsro128", m_vsro128);

static xts_mres_t m_vsl(VI_PARAMS) { VI_UNUSED; return vbits(0, w, in, out); }
XTS_MODEL("vsl", m_vsl);

static xts_mres_t m_vsr(VI_PARAMS) { VI_UNUSED; return vbits(1, w, in, out); }
XTS_MODEL("vsr", m_vsr);
