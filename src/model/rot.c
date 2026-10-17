/* Integer model (mint): rotates and shifts. Word rotates rotate the doubled low word (rlwinm etc.), masks follow the
 * architecture's MASK(b,e) with wrap-around (IBM bit numbering, bit 0 = MSB). */
#include <xts/model.h>

#define HANDLER(name) static xts_mres_t name(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, \
                                              xts_state_t *out)
#define UNUSED (void)d; (void)m
#define GS (in->gpr[XM_RS(w)])
#define GB (in->gpr[XM_RB(w)])

static uint64_t mask64(int b, int e)
{
  uint64_t hi = ~0ull >> b, lo = ~0ull << (63 - e);
  return b <= e ? (hi & lo) : (hi | lo);
}
static uint64_t rotl64(uint64_t v, unsigned n) { n &= 63; return n ? (v << n) | (v >> (64 - n)) : v; }
static uint64_t rotl32x2(uint64_t v, unsigned n)   /* ROTL32(low word) || ROTL32(low word) */
{
  uint32_t x = (uint32_t)v;
  n &= 31;
  if (n) x = (x << n) | (x >> (32 - n));
  return ((uint64_t)x << 32) | x;
}

static xts_mres_t fin(uint32_t w, const xts_state_t *in, xts_state_t *out, uint64_t r)
{
  out->gpr[XM_RA(w)] = r;
  if (XM_RC_BIT(w)) xm_cr0(out, r, xm_sf(in));
  return XTS_M_OK;
}

/* M-form fields */
#define SH5(w) XM_BITS(w, 11, 5)
#define MB5(w) XM_BITS(w, 6, 5)
#define ME5(w) XM_BITS(w, 1, 5)
/* MD/MDS-form fields: sh = sh[0:4] || sh5, mb/me = v[1:5] || v[0] */
#define SH6(w) (XM_BITS(w, 11, 5) | (XM_BITS(w, 1, 1) << 5))
#define MB6(w) (XM_BITS(w, 6, 5) | (XM_BITS(w, 5, 1) << 5))

HANDLER(m_rlwinmx) { UNUSED; return fin(w, in, out, rotl32x2(GS, SH5(w)) & mask64(MB5(w) + 32, ME5(w) + 32)); }
XTS_MODEL("rlwinmx", m_rlwinmx);
HANDLER(m_rlwnmx) { UNUSED; return fin(w, in, out, rotl32x2(GS, (unsigned)GB) & mask64(MB5(w) + 32, ME5(w) + 32)); }
XTS_MODEL("rlwnmx", m_rlwnmx);
HANDLER(m_rlwimix)
{
  UNUSED;
  uint64_t mk = mask64(MB5(w) + 32, ME5(w) + 32);
  return fin(w, in, out, (rotl32x2(GS, SH5(w)) & mk) | (in->gpr[XM_RA(w)] & ~mk));
}
XTS_MODEL("rlwimix", m_rlwimix);

HANDLER(m_rldiclx) { UNUSED; return fin(w, in, out, rotl64(GS, SH6(w)) & mask64(MB6(w), 63)); }
XTS_MODEL("rldiclx", m_rldiclx);
HANDLER(m_rldicrx) { UNUSED; return fin(w, in, out, rotl64(GS, SH6(w)) & mask64(0, MB6(w))); }   /* field holds ME */
XTS_MODEL("rldicrx", m_rldicrx);
HANDLER(m_rldicx) { UNUSED; return fin(w, in, out, rotl64(GS, SH6(w)) & mask64(MB6(w), 63 - SH6(w))); }
XTS_MODEL("rldicx", m_rldicx);
HANDLER(m_rldimix)
{
  UNUSED;
  uint64_t mk = mask64(MB6(w), 63 - SH6(w));
  return fin(w, in, out, (rotl64(GS, SH6(w)) & mk) | (in->gpr[XM_RA(w)] & ~mk));
}
XTS_MODEL("rldimix", m_rldimix);
HANDLER(m_rldclx) { UNUSED; return fin(w, in, out, rotl64(GS, (unsigned)GB & 63) & mask64(MB6(w), 63)); }
XTS_MODEL("rldclx", m_rldclx);
HANDLER(m_rldcrx) { UNUSED; return fin(w, in, out, rotl64(GS, (unsigned)GB & 63) & mask64(0, MB6(w))); }
XTS_MODEL("rldcrx", m_rldcrx);

/* ---- shifts (the 6-bit amount in RB: bit 5 set = shift out everything) ---- */
HANDLER(m_slwx)
{
  UNUSED;
  unsigned n = (unsigned)GB & 63;
  return fin(w, in, out, n >= 32 ? 0 : (uint64_t)((uint32_t)GS << n));
}
XTS_MODEL("slwx", m_slwx);
HANDLER(m_srwx)
{
  UNUSED;
  unsigned n = (unsigned)GB & 63;
  return fin(w, in, out, n >= 32 ? 0 : (uint64_t)((uint32_t)GS >> n));
}
XTS_MODEL("srwx", m_srwx);
HANDLER(m_sldx)
{
  UNUSED;
  unsigned n = (unsigned)GB & 127;
  return fin(w, in, out, n >= 64 ? 0 : GS << n);
}
XTS_MODEL("sldx", m_sldx);
HANDLER(m_srdx)
{
  UNUSED;
  unsigned n = (unsigned)GB & 127;
  return fin(w, in, out, n >= 64 ? 0 : GS >> n);
}
XTS_MODEL("srdx", m_srdx);

static xts_mres_t sraw_n(uint32_t w, const xts_state_t *in, xts_state_t *out, unsigned n)   /* n 0..63 */
{
  int32_t v = (int32_t)GS;
  int64_t r = n > 31 ? (v < 0 ? -1 : 0) : (int64_t)(v >> n);
  int ca = v < 0 && (n > 31 || (n && ((uint32_t)v << (32 - n)) != 0));
  xm_set_ca(out, ca);
  return fin(w, in, out, (uint64_t)r);
}
HANDLER(m_srawx) { UNUSED; return sraw_n(w, in, out, (unsigned)GB & 63); }
XTS_MODEL("srawx", m_srawx);
HANDLER(m_srawix) { UNUSED; return sraw_n(w, in, out, SH5(w)); }
XTS_MODEL("srawix", m_srawix);

static xts_mres_t srad_n(uint32_t w, const xts_state_t *in, xts_state_t *out, unsigned n)   /* n 0..127 */
{
  int64_t v = (int64_t)GS;
  int64_t r = n > 63 ? (v < 0 ? -1 : 0) : (v >> n);
  int ca = v < 0 && (n > 63 ? 1 : n != 0 && ((uint64_t)v << (64 - n)) != 0);
  xm_set_ca(out, ca);
  return fin(w, in, out, (uint64_t)r);
}
HANDLER(m_sradx) { UNUSED; return srad_n(w, in, out, (unsigned)GB & 127); }
XTS_MODEL("sradx", m_sradx);
HANDLER(m_sradix) { UNUSED; return srad_n(w, in, out, SH6(w)); }
XTS_MODEL("sradix", m_sradix);
