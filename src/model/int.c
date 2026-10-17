/* Integer model: add/sub/carry/OV/SO/CR0, logical, mul/div.
 * Phase A ships addx and orx as the worked examples of the handler interface (validated against results4 by
 * tools/check_model.py, all Rc/OE variants). The owner of this file adds the rest of the family. */
#include <xts/model.h>

/* add[o][.] RD,RA,RB */
static xts_mres_t m_addx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  int sf = xm_sf(in);
  uint64_t a = in->gpr[XM_RA(w)], b = in->gpr[XM_RB(w)], r = a + b;
  out->gpr[XM_RD(w)] = r;
  if (XM_OE_BIT(w)) xm_set_ov(out, xm_add_ov(a, b, r, sf));
  if (XM_RC_BIT(w)) xm_cr0(out, r, sf);
  return XTS_M_OK;
}
XTS_MODEL("addx", m_addx);

/* or[.] RA,RS,RB (or 1,1,1 / 2,2,2 / 3,3,3 are thread-priority hints with no architected effect beyond the or) */
static xts_mres_t m_orx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)d; (void)m;
  uint64_t r = in->gpr[XM_RS(w)] | in->gpr[XM_RB(w)];
  out->gpr[XM_RA(w)] = r;
  if (XM_RC_BIT(w)) xm_cr0(out, r, xm_sf(in));
  return XTS_M_OK;
}
XTS_MODEL("orx", m_orx);
