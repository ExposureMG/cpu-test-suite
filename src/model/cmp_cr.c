/* Integer model (mint): condition-register logic and moves, mfspr/mtspr for XER/LR/CTR. */
#include <xts/model.h>

#define HANDLER(name) static xts_mres_t name(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, \
                                              xts_state_t *out)
#define UNUSED (void)d; (void)m

static inline int crbit(const xts_state_t *s, unsigned i) { return (int)(((uint32_t)s->cr >> (31 - i)) & 1); }

HANDLER(m_mcrf)
{
  UNUSED;
  xm_set_cr_field(out, (int)XM_BITS(w, 23, 3), xm_cr_field(in, (int)XM_BITS(w, 18, 3)));
  return XTS_M_OK;
}
XTS_MODEL("mcrf", m_mcrf);

static xts_mres_t crop(uint32_t w, const xts_state_t *in, xts_state_t *out, int (*f)(int, int))
{
  unsigned bt = XM_BITS(w, 21, 5), ba = XM_BITS(w, 16, 5), bb = XM_BITS(w, 11, 5);
  int r = f(crbit(in, ba), crbit(in, bb)) & 1;
  uint32_t cr = (uint32_t)in->cr;
  cr = (cr & ~(1u << (31 - bt))) | ((uint32_t)r << (31 - bt));
  out->cr = cr;
  return XTS_M_OK;
}
static int f_and(int a, int b) { return a & b; }
static int f_or(int a, int b) { return a | b; }
static int f_xor(int a, int b) { return a ^ b; }
static int f_nand(int a, int b) { return ~(a & b); }
static int f_nor(int a, int b) { return ~(a | b); }
static int f_eqv(int a, int b) { return ~(a ^ b); }
static int f_andc(int a, int b) { return a & ~b; }
static int f_orc(int a, int b) { return a | ~b; }
HANDLER(m_crand) { UNUSED; return crop(w, in, out, f_and); }
XTS_MODEL("crand", m_crand);
HANDLER(m_cror) { UNUSED; return crop(w, in, out, f_or); }
XTS_MODEL("cror", m_cror);
HANDLER(m_crxor) { UNUSED; return crop(w, in, out, f_xor); }
XTS_MODEL("crxor", m_crxor);
HANDLER(m_crnand) { UNUSED; return crop(w, in, out, f_nand); }
XTS_MODEL("crnand", m_crnand);
HANDLER(m_crnor) { UNUSED; return crop(w, in, out, f_nor); }
XTS_MODEL("crnor", m_crnor);
HANDLER(m_creqv) { UNUSED; return crop(w, in, out, f_eqv); }
XTS_MODEL("creqv", m_creqv);
HANDLER(m_crandc) { UNUSED; return crop(w, in, out, f_andc); }
XTS_MODEL("crandc", m_crandc);
HANDLER(m_crorc) { UNUSED; return crop(w, in, out, f_orc); }
XTS_MODEL("crorc", m_crorc);

/* FXM bit 0 (mask 0x80) = CR field 0 */
static uint32_t fxm_mask(unsigned fxm)
{
  uint32_t m = 0;
  for (int i = 0; i < 8; i++)
    if (fxm & (0x80u >> i)) m |= 0xFu << (28 - 4 * i);
  return m;
}

HANDLER(m_mfcr)
{
  UNUSED;
  out->gpr[XM_RD(w)] = (uint32_t)in->cr;
  return XTS_M_OK;
}
XTS_MODEL("mfcr", m_mfcr);

/* Xenon (results4): mfocrf ignores FXM and returns the whole CR (every multi-bit FXM case with a non-zero CR). Single-bit
 * FXM never occurred with a non-zero CR in results4: assumed identical. */
HANDLER(m_mfocrf)
{
  UNUSED;
  out->gpr[XM_RD(w)] = (uint32_t)in->cr;
  return XTS_M_OK;
}
XTS_MODEL("mfocrf", m_mfocrf);

static xts_mres_t mtcr_mask(uint32_t w, const xts_state_t *in, xts_state_t *out)
{
  uint32_t mk = fxm_mask(XM_BITS(w, 12, 8));
  out->cr = ((uint32_t)in->cr & ~mk) | ((uint32_t)in->gpr[XM_RS(w)] & mk);
  return XTS_M_OK;
}
HANDLER(m_mtcrf) { UNUSED; return mtcr_mask(w, in, out); }
XTS_MODEL("mtcrf", m_mtcrf);

/* Xenon (results4): mtocrf with several FXM bits writes only the most significant selected field (CR field 0 first);
 * FXM = 0 changes nothing. */
HANDLER(m_mtocrf)
{
  UNUSED;
  unsigned fxm = XM_BITS(w, 12, 8);
  for (int f = 0; f < 8; f++)
    if (fxm & (0x80u >> f)) {
      xm_set_cr_field(out, f, (uint32_t)(in->gpr[XM_RS(w)] >> (28 - 4 * f)) & 0xF);
      break;
    }
  return XTS_M_OK;
}
XTS_MODEL("mtocrf", m_mtocrf);

/* ---- SPR moves: XER(1), LR(8), CTR(9). Everything else (and the time base) is not modelled. ---- */
#define SPR_NUM(w) (XM_BITS(w, 16, 5) | (XM_BITS(w, 11, 5) << 5))

HANDLER(m_mfspr)
{
  UNUSED;
  switch (SPR_NUM(w)) {
  case 1: out->gpr[XM_RD(w)] = in->xer; return XTS_M_OK;
  case 8: out->gpr[XM_RD(w)] = in->lr; return XTS_M_OK;
  case 9: out->gpr[XM_RD(w)] = in->ctr; return XTS_M_OK;
  }
  return XTS_M_UNIMPLEMENTED;
}
XTS_MODEL("mfspr", m_mfspr);

HANDLER(m_mtspr)
{
  UNUSED;
  uint64_t v = in->gpr[XM_RS(w)];
  switch (SPR_NUM(w)) {
  case 1: out->xer = v & 0xE000007Full; return XTS_M_OK;   /* SO OV CA + byte count; the rest reads as 0 */
  case 8: out->lr = v; return XTS_M_OK;
  case 9: out->ctr = v; return XTS_M_OK;
  }
  return XTS_M_UNIMPLEMENTED;
}
XTS_MODEL("mtspr", m_mtspr);

/* mftb is deliberately not registered: the time base is nondeterministic (the dispatcher answers UNIMPLEMENTED). */
