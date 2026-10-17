/* Cell/Xenon vector left/right loads and stores (lvlx lvrx stvlx stvrx and their LRU-hint "l" forms, plus the VMX128
 * forms). 24 results4 cases per mnemonic, all of which the architecture description below reproduces.
 *
 *   sh = EA & 15 (the EA is NOT aligned down; the quadword is the one containing EA)
 *   lvlx  vD: bytes EA..end-of-quadword (16-sh of them) -> vD bytes 0..15-sh, the remaining bytes of vD are zero
 *   lvrx  vD: bytes quadword-start..EA-1 (sh of them)  -> vD bytes 16-sh..15, the other bytes are zero (sh=0: all zero)
 *   stvlx vS: vS bytes 0..15-sh -> memory EA..end-of-quadword
 *   stvrx vS: vS bytes 16-sh..15 -> memory quadword-start..EA-1 (sh=0: nothing)
 * Nothing is accessed outside the bytes named, so a case at a region's very end still works.
 */
#include <string.h>
#include <xts/enc.h>
#include <xts/model.h>
#include "ldst_priv.h"

int ml_lvlx(xts_mem_t *m, uint64_t ea, xts_vr_t *vd)
{
  unsigned sh = (unsigned)(ea & 15), n = 16 - sh;
  uint8_t *p = xts_mem_ptr(m, ea, n);
  if (!p) return XTS_M_DSI;
  uint8_t q[16] = { 0 };
  memcpy(q, p, n);
  *vd = xts_vr_load(q);
  return 0;
}

int ml_lvrx(xts_mem_t *m, uint64_t ea, xts_vr_t *vd)
{
  unsigned sh = (unsigned)(ea & 15);
  uint8_t q[16] = { 0 };
  if (sh) {
    uint8_t *p = xts_mem_ptr(m, ea & ~15ull, sh);
    if (!p) return XTS_M_DSI;
    memcpy(q + 16 - sh, p, sh);
  }
  *vd = xts_vr_load(q);
  return 0;
}

int ml_stvlx(xts_mem_t *m, uint64_t ea, const xts_vr_t *vs)
{
  unsigned sh = (unsigned)(ea & 15), n = 16 - sh;
  uint8_t *p = xts_mem_ptr(m, ea, n);
  if (!p) return XTS_M_DSI;
  uint8_t q[16];
  xts_vr_store(q, vs);
  memcpy(p, q, n);
  return 0;
}

int ml_stvrx(xts_mem_t *m, uint64_t ea, const xts_vr_t *vs)
{
  unsigned sh = (unsigned)(ea & 15);
  if (!sh) return 0;
  uint8_t *p = xts_mem_ptr(m, ea & ~15ull, sh);
  if (!p) return XTS_M_DSI;
  uint8_t q[16];
  xts_vr_store(q, vs);
  memcpy(p, q + 16 - sh, sh);
  return 0;
}

static uint64_t cea(const xts_insn_t *d, uint32_t w, const xts_state_t *in) { return ml_ea_x_ops(d, w, in, 1, 2); }

static xts_mres_t m_lvlx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  return (xts_mres_t)ml_lvlx(m, cea(d, w, in), &out->vr[ml_vreg(d, w)]);
}
static xts_mres_t m_lvrx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  return (xts_mres_t)ml_lvrx(m, cea(d, w, in), &out->vr[ml_vreg(d, w)]);
}
static xts_mres_t m_stvlx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)out;
  return (xts_mres_t)ml_stvlx(m, cea(d, w, in), &in->vr[ml_vreg(d, w)]);
}
static xts_mres_t m_stvrx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)out;
  return (xts_mres_t)ml_stvrx(m, cea(d, w, in), &in->vr[ml_vreg(d, w)]);
}

XTS_MODEL("lvlx", m_lvlx);
XTS_MODEL("lvlxl", m_lvlx);
XTS_MODEL("lvlx128", m_lvlx);
XTS_MODEL("lvlxl128", m_lvlx);
XTS_MODEL("lvrx", m_lvrx);
XTS_MODEL("lvrxl", m_lvrx);
XTS_MODEL("lvrx128", m_lvrx);
XTS_MODEL("lvrxl128", m_lvrx);
XTS_MODEL("stvlx", m_stvlx);
XTS_MODEL("stvlxl", m_stvlx);
XTS_MODEL("stvlx128", m_stvlx);
XTS_MODEL("stvlxl128", m_stvlx);
XTS_MODEL("stvrx", m_stvrx);
XTS_MODEL("stvrxl", m_stvrx);
XTS_MODEL("stvrx128", m_stvrx);
XTS_MODEL("stvrxl128", m_stvrx);
