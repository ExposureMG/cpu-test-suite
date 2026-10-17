/* Integer model (mint): add/subtract with carry, negate, multiply, divide, logical, immediate forms, extends, counts,
 * compares. Rotates and shifts are in rot.c, CR/SPR moves in cmp_cr.c, traps in trap.c.
 * Mode handling: MSR[SF]=0 changes CR0/compare width and the carry/overflow position; the 64-bit result is still
 * written in full. Undefined-result policy (mul/div) is documented in KNOWN.md "## int (mint)". */
#include <xts/model.h>

#define HANDLER(name) static xts_mres_t name(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, \
                                              xts_state_t *out)
#define UNUSED (void)d; (void)m
#define GA (in->gpr[XM_RA(w)])
#define GB (in->gpr[XM_RB(w)])
#define GS (in->gpr[XM_RS(w)])

/* portable 64x64 -> 128 multiply (the 32-bit console compiler has no __int128) */
static void mulu64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
  uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
  uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
  *lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
  *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
static void muls64(int64_t a, int64_t b, int64_t *hi, uint64_t *lo)
{
  uint64_t h;
  mulu64((uint64_t)a, (uint64_t)b, &h, lo);
  if (a < 0) h -= (uint64_t)b;
  if (b < 0) h -= (uint64_t)a;
  *hi = (int64_t)h;
}

/* OE / Rc tail of the XO-form instructions */
static void fin_xo(xts_state_t *out, uint32_t w, int sf, uint64_t r, int ov)
{
  if (XM_OE_BIT(w)) xm_set_ov(out, ov);
  if (XM_RC_BIT(w)) xm_cr0(out, r, sf);
}

/* ---- addition / subtraction family ---- */
HANDLER(m_addi)
{
  UNUSED;
  out->gpr[XM_RD(w)] = (XM_RA(w) ? GA : 0) + (uint64_t)XM_SIMM16(w);
  return XTS_M_OK;
}
XTS_MODEL("addi", m_addi);

HANDLER(m_addis)
{
  UNUSED;
  out->gpr[XM_RD(w)] = (XM_RA(w) ? GA : 0) + ((uint64_t)XM_SIMM16(w) << 16);
  return XTS_M_OK;
}
XTS_MODEL("addis", m_addis);

static xts_mres_t addic_common(uint32_t w, const xts_state_t *in, xts_state_t *out, int rc)
{
  int sf = xm_sf(in);
  uint64_t a = GA, b = (uint64_t)XM_SIMM16(w), r = a + b;
  out->gpr[XM_RD(w)] = r;
  xm_set_ca(out, xm_add_ca(a, b, 0, sf));
  if (rc) xm_cr0(out, r, sf);
  return XTS_M_OK;
}
HANDLER(m_addic) { UNUSED; return addic_common(w, in, out, 0); }
XTS_MODEL("addic", m_addic);
HANDLER(m_addic_) { UNUSED; return addic_common(w, in, out, 1); }
XTS_MODEL("addic.", m_addic_);

/* subfic RD,RA,SIMM = ~RA + SIMM + 1 */
HANDLER(m_subficx)
{
  UNUSED;
  int sf = xm_sf(in);
  uint64_t a = ~GA, b = (uint64_t)XM_SIMM16(w);
  out->gpr[XM_RD(w)] = a + b + 1;
  xm_set_ca(out, xm_add_ca(a, b, 1, sf));
  return XTS_M_OK;
}
XTS_MODEL("subficx", m_subficx);

/* a + b + cin; CA updated unless this is plain add/subf */
static xts_mres_t addcarry(uint32_t w, const xts_state_t *in, xts_state_t *out, uint64_t a, uint64_t b, int cin,
                           int set_ca)
{
  int sf = xm_sf(in);
  uint64_t r = a + b + (uint64_t)cin;
  out->gpr[XM_RD(w)] = r;
  if (set_ca) xm_set_ca(out, xm_add_ca(a, b, cin, sf));
  fin_xo(out, w, sf, r, xm_add_ov(a, b, r, sf));
  return XTS_M_OK;
}
HANDLER(m_addcx) { UNUSED; return addcarry(w, in, out, GA, GB, 0, 1); }
XTS_MODEL("addcx", m_addcx);
HANDLER(m_addex) { UNUSED; return addcarry(w, in, out, GA, GB, xm_xer_ca(in), 1); }
XTS_MODEL("addex", m_addex);
HANDLER(m_addzex) { UNUSED; return addcarry(w, in, out, GA, 0, xm_xer_ca(in), 1); }
XTS_MODEL("addzex", m_addzex);
HANDLER(m_addmex) { UNUSED; return addcarry(w, in, out, GA, ~0ull, xm_xer_ca(in), 1); }
XTS_MODEL("addmex", m_addmex);
HANDLER(m_subfx) { UNUSED; return addcarry(w, in, out, ~GA, GB, 1, 0); }
XTS_MODEL("subfx", m_subfx);
HANDLER(m_subfcx) { UNUSED; return addcarry(w, in, out, ~GA, GB, 1, 1); }
XTS_MODEL("subfcx", m_subfcx);
HANDLER(m_subfex) { UNUSED; return addcarry(w, in, out, ~GA, GB, xm_xer_ca(in), 1); }
XTS_MODEL("subfex", m_subfex);
HANDLER(m_subfzex) { UNUSED; return addcarry(w, in, out, ~GA, 0, xm_xer_ca(in), 1); }
XTS_MODEL("subfzex", m_subfzex);
HANDLER(m_subfmex) { UNUSED; return addcarry(w, in, out, ~GA, ~0ull, xm_xer_ca(in), 1); }
XTS_MODEL("subfmex", m_subfmex);

HANDLER(m_negx)
{
  UNUSED;
  int sf = xm_sf(in);
  uint64_t a = GA, r = 0 - a;
  out->gpr[XM_RD(w)] = r;
  fin_xo(out, w, sf, r, sf ? a == 0x8000000000000000ull : (uint32_t)a == 0x80000000u);
  return XTS_M_OK;
}
XTS_MODEL("negx", m_negx);

/* ---- multiply ---- */
HANDLER(m_mulli)
{
  UNUSED;
  out->gpr[XM_RD(w)] = GA * (uint64_t)XM_SIMM16(w);
  return XTS_M_OK;
}
XTS_MODEL("mulli", m_mulli);

HANDLER(m_mullwx)
{
  UNUSED;
  int sf = xm_sf(in);
  int64_t p = (int64_t)(int32_t)GA * (int64_t)(int32_t)GB;
  out->gpr[XM_RD(w)] = (uint64_t)p;
  fin_xo(out, w, sf, (uint64_t)p, p != (int64_t)(int32_t)p);
  return XTS_M_OK;
}
XTS_MODEL("mullwx", m_mullwx);

HANDLER(m_mulldx)
{
  UNUSED;
  int sf = xm_sf(in);
  int64_t hi;
  uint64_t r;
  muls64((int64_t)GA, (int64_t)GB, &hi, &r);
  out->gpr[XM_RD(w)] = r;
  fin_xo(out, w, sf, r, hi != ((int64_t)r >> 63));
  return XTS_M_OK;
}
XTS_MODEL("mulldx", m_mulldx);

/* mulh*: the architecture leaves RT[0:31] (word forms) undefined; see KNOWN.md for what Xenon does. OE is a
 * reserved bit: it never changes XER here. */
static xts_mres_t mulh_word(uint32_t w, const xts_state_t *in, xts_state_t *out, int is_signed)
{
  int sf = xm_sf(in);
  uint64_t r;
  if (XM_OE_BIT(w) && XM_RC_BIT(w)) return XTS_M_ILLEGAL;   /* reserved form: illegal on Xenon (OpenXenos sweep) */
  if (is_signed) r = (uint64_t)(((int64_t)(int32_t)GA * (int64_t)(int32_t)GB) >> 32);
  else r = ((uint64_t)(uint32_t)GA * (uint64_t)(uint32_t)GB) >> 32;
  out->gpr[XM_RD(w)] = r;
  if (XM_RC_BIT(w)) xm_cr0(out, r, sf);
  return XTS_M_OK;
}
HANDLER(m_mulhwx) { UNUSED; return mulh_word(w, in, out, 1); }
XTS_MODEL("mulhwx", m_mulhwx);
HANDLER(m_mulhwux) { UNUSED; return mulh_word(w, in, out, 0); }
XTS_MODEL("mulhwux", m_mulhwux);

HANDLER(m_mulhdx)
{
  UNUSED;
  if (XM_OE_BIT(w) && XM_RC_BIT(w)) return XTS_M_ILLEGAL;
  int64_t hi;
  uint64_t lo;
  muls64((int64_t)GA, (int64_t)GB, &hi, &lo);
  uint64_t r = (uint64_t)hi;
  out->gpr[XM_RD(w)] = r;
  if (XM_RC_BIT(w)) xm_cr0(out, r, xm_sf(in));
  return XTS_M_OK;
}
XTS_MODEL("mulhdx", m_mulhdx);

HANDLER(m_mulhdux)
{
  UNUSED;
  if (XM_OE_BIT(w) && XM_RC_BIT(w)) return XTS_M_ILLEGAL;
  uint64_t r, lo;
  mulu64(GA, GB, &r, &lo);
  out->gpr[XM_RD(w)] = r;
  if (XM_RC_BIT(w)) xm_cr0(out, r, xm_sf(in));
  return XTS_M_OK;
}
XTS_MODEL("mulhdux", m_mulhdux);

/* ---- divide: zero divisor / overflow leave RT undefined and set OV (OE form); policy per KNOWN.md ---- */
HANDLER(m_divwx)
{
  UNUSED;
  int sf = xm_sf(in);
  int32_t a = (int32_t)GA, b = (int32_t)GB;
  int ov = b == 0 || (a == INT32_MIN && b == -1);
  uint32_t q = ov ? 0 : (uint32_t)(a / b);
  out->gpr[XM_RD(w)] = q;
  fin_xo(out, w, sf, q, ov);
  return XTS_M_OK;
}
XTS_MODEL("divwx", m_divwx);

HANDLER(m_divwux)
{
  UNUSED;
  int sf = xm_sf(in);
  uint32_t a = (uint32_t)GA, b = (uint32_t)GB;
  int ov = b == 0;
  uint32_t q = ov ? 0 : a / b;
  out->gpr[XM_RD(w)] = q;
  fin_xo(out, w, sf, q, ov);
  return XTS_M_OK;
}
XTS_MODEL("divwux", m_divwux);

HANDLER(m_divdx)
{
  UNUSED;
  int sf = xm_sf(in);
  int64_t a = (int64_t)GA, b = (int64_t)GB;
  int ov = b == 0 || (a == INT64_MIN && b == -1);
  uint64_t q = ov ? 0 : (uint64_t)(a / b);
  out->gpr[XM_RD(w)] = q;
  fin_xo(out, w, sf, q, ov);
  return XTS_M_OK;
}
XTS_MODEL("divdx", m_divdx);

HANDLER(m_divdux)
{
  UNUSED;
  int sf = xm_sf(in);
  uint64_t a = GA, b = GB;
  int ov = b == 0;
  uint64_t q = ov ? 0 : a / b;
  out->gpr[XM_RD(w)] = q;
  fin_xo(out, w, sf, q, ov);
  return XTS_M_OK;
}
XTS_MODEL("divdux", m_divdux);

/* ---- logical ---- */
static xts_mres_t logic_rc(uint32_t w, const xts_state_t *in, xts_state_t *out, uint64_t r)
{
  out->gpr[XM_RA(w)] = r;
  if (XM_RC_BIT(w)) xm_cr0(out, r, xm_sf(in));
  return XTS_M_OK;
}
HANDLER(m_andx) { UNUSED; return logic_rc(w, in, out, GS & GB); }
XTS_MODEL("andx", m_andx);
HANDLER(m_andcx) { UNUSED; return logic_rc(w, in, out, GS & ~GB); }
XTS_MODEL("andcx", m_andcx);
HANDLER(m_norx) { UNUSED; return logic_rc(w, in, out, ~(GS | GB)); }
XTS_MODEL("norx", m_norx);
HANDLER(m_nandx) { UNUSED; return logic_rc(w, in, out, ~(GS & GB)); }
XTS_MODEL("nandx", m_nandx);
HANDLER(m_xorx) { UNUSED; return logic_rc(w, in, out, GS ^ GB); }
XTS_MODEL("xorx", m_xorx);
HANDLER(m_eqvx) { UNUSED; return logic_rc(w, in, out, ~(GS ^ GB)); }
XTS_MODEL("eqvx", m_eqvx);
HANDLER(m_orcx) { UNUSED; return logic_rc(w, in, out, GS | ~GB); }
XTS_MODEL("orcx", m_orcx);

HANDLER(m_extsbx) { UNUSED; return logic_rc(w, in, out, (uint64_t)(int64_t)(int8_t)GS); }
XTS_MODEL("extsbx", m_extsbx);
HANDLER(m_extshx) { UNUSED; return logic_rc(w, in, out, (uint64_t)(int64_t)(int16_t)GS); }
XTS_MODEL("extshx", m_extshx);
HANDLER(m_extswx) { UNUSED; return logic_rc(w, in, out, (uint64_t)(int64_t)(int32_t)GS); }
XTS_MODEL("extswx", m_extswx);
HANDLER(m_cntlzwx) { UNUSED; uint32_t v = (uint32_t)GS; return logic_rc(w, in, out, v ? (uint64_t)__builtin_clz(v) : 32); }
XTS_MODEL("cntlzwx", m_cntlzwx);
HANDLER(m_cntlzdx) { UNUSED; uint64_t v = GS; return logic_rc(w, in, out, v ? (uint64_t)__builtin_clzll(v) : 64); }
XTS_MODEL("cntlzdx", m_cntlzdx);

/* ori/oris/xori/xoris: no CR; andi./andis. always set CR0 */
HANDLER(m_ori) { UNUSED; out->gpr[XM_RA(w)] = GS | XM_UIMM16(w); return XTS_M_OK; }
XTS_MODEL("ori", m_ori);
HANDLER(m_oris) { UNUSED; out->gpr[XM_RA(w)] = GS | (XM_UIMM16(w) << 16); return XTS_M_OK; }
XTS_MODEL("oris", m_oris);
HANDLER(m_xori) { UNUSED; out->gpr[XM_RA(w)] = GS ^ XM_UIMM16(w); return XTS_M_OK; }
XTS_MODEL("xori", m_xori);
HANDLER(m_xoris) { UNUSED; out->gpr[XM_RA(w)] = GS ^ (XM_UIMM16(w) << 16); return XTS_M_OK; }
XTS_MODEL("xoris", m_xoris);
HANDLER(m_andi_)
{
  UNUSED;
  uint64_t r = GS & XM_UIMM16(w);
  out->gpr[XM_RA(w)] = r;
  xm_cr0(out, r, xm_sf(in));
  return XTS_M_OK;
}
XTS_MODEL("andi.", m_andi_);
HANDLER(m_andis_)
{
  UNUSED;
  uint64_t r = GS & (XM_UIMM16(w) << 16);
  out->gpr[XM_RA(w)] = r;
  xm_cr0(out, r, xm_sf(in));
  return XTS_M_OK;
}
XTS_MODEL("andis.", m_andis_);

/* ---- compares: BF = bits 6-8, L = bit 10; L=0 compares the low words (sign- or zero-extended) ---- */
static xts_mres_t compare(uint32_t w, const xts_state_t *in, xts_state_t *out, uint64_t a, uint64_t b, int is_signed)
{
  int l = (int)XM_BITS(w, 21, 1);
  if (!l) {
    if (is_signed) { a = xm_exts32(a); b = xm_exts32(b); }
    else { a = (uint32_t)a; b = (uint32_t)b; }
  }
  uint32_t f;
  if (is_signed) f = (int64_t)a < (int64_t)b ? 8 : (int64_t)a > (int64_t)b ? 4 : 2;
  else f = a < b ? 8 : a > b ? 4 : 2;
  if (xm_xer_so(in)) f |= 1;
  xm_set_cr_field(out, (int)XM_BITS(w, 23, 3), f);
  return XTS_M_OK;
}
HANDLER(m_cmp) { UNUSED; return compare(w, in, out, GA, GB, 1); }
XTS_MODEL("cmp", m_cmp);
HANDLER(m_cmpl) { UNUSED; return compare(w, in, out, GA, GB, 0); }
XTS_MODEL("cmpl", m_cmpl);
HANDLER(m_cmpi) { UNUSED; return compare(w, in, out, GA, (uint64_t)XM_SIMM16(w), 1); }
XTS_MODEL("cmpi", m_cmpi);
HANDLER(m_cmpli) { UNUSED; return compare(w, in, out, GA, XM_UIMM16(w), 0); }
XTS_MODEL("cmpli", m_cmpli);
