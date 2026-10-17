/* Integer model (mint): trap instructions. TO bits (value): 16 signed <, 8 signed >, 4 =, 2 unsigned <, 1 unsigned >.
 * A taken trap returns XTS_M_TRAP (dispatcher restores the input state); otherwise the instruction has no effect. */
#include <xts/model.h>

#define HANDLER(name) static xts_mres_t name(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, \
                                              xts_state_t *out)
#define UNUSED (void)d; (void)m; (void)out

static xts_mres_t trap_test(uint32_t w, uint64_t a, uint64_t b)
{
  uint32_t to = XM_BITS(w, 21, 5);
  int hit = ((to & 16) && (int64_t)a < (int64_t)b) || ((to & 8) && (int64_t)a > (int64_t)b) || ((to & 4) && a == b) ||
            ((to & 2) && a < b) || ((to & 1) && a > b);
  return hit ? XTS_M_TRAP : XTS_M_OK;
}

HANDLER(m_tw) { UNUSED; return trap_test(w, xm_exts32(in->gpr[XM_RA(w)]), xm_exts32(in->gpr[XM_RB(w)])); }
XTS_MODEL("tw", m_tw);
HANDLER(m_td) { UNUSED; return trap_test(w, in->gpr[XM_RA(w)], in->gpr[XM_RB(w)]); }
XTS_MODEL("td", m_td);
HANDLER(m_twi) { UNUSED; return trap_test(w, xm_exts32(in->gpr[XM_RA(w)]), (uint64_t)XM_SIMM16(w)); }
XTS_MODEL("twi", m_twi);
HANDLER(m_tdi) { UNUSED; return trap_test(w, in->gpr[XM_RA(w)], (uint64_t)XM_SIMM16(w)); }
XTS_MODEL("tdi", m_tdi);
