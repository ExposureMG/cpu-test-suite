/* AltiVec / VMX128 vector memory: lvx stvx lvxl stvxl, element loads/stores, lvsl lvsr, and the VMX128 forms
 * (VD128/VS128 from the descriptor's register field). Cell left/right forms: cellvmx.c.
 *
 * Hardware rules (results4): the EA's low 4 bits are ignored by lvx/stvx (quadword access), element accesses use the
 * low bits only to pick the element; vector loads/stores never take an alignment interrupt. The element loads
 * lvebx/lvehx/lvewx (and lvewx128) load the WHOLE aligned quadword on Xenon (the architecture leaves the other
 * elements undefined; the hardware replaces all 16 bytes with the memory quadword) - modelled as such.
 */
#include <xts/enc.h>
#include <xts/model.h>
#include "ldst_priv.h"

static uint64_t vea(const xts_insn_t *d, uint32_t w, const xts_state_t *in) { return ml_ea_x_ops(d, w, in, 1, 2); }

static xts_mres_t vload(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  uint8_t *p = xts_mem_ptr(m, vea(d, w, in) & ~15ull, 16);
  if (!p) return XTS_M_DSI;
  out->vr[ml_vreg(d, w)] = xts_vr_load(p);
  return XTS_M_OK;
}

static xts_mres_t vstore(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)out;
  uint8_t *p = xts_mem_ptr(m, vea(d, w, in) & ~15ull, 16);
  if (!p) return XTS_M_DSI;
  xts_vr_store(p, &in->vr[ml_vreg(d, w)]);
  return XTS_M_OK;
}

/* stvebx / stvehx / stvewx: the element selected by the EA's low bits is stored at the (size-aligned) EA */
static xts_mres_t vstore_elem(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, int size)
{
  uint64_t ea = vea(d, w, in) & ~(uint64_t)(size - 1);
  uint8_t *p = xts_mem_ptr(m, ea, (uint32_t)size);
  if (!p) return XTS_M_DSI;
  uint8_t q[16];
  xts_vr_store(q, &in->vr[ml_vreg(d, w)]);
  for (int i = 0; i < size; i++) p[i] = q[(ea & 15) + (uint64_t)i];
  return XTS_M_OK;
}
static xts_mres_t m_stvebx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)out;
  return vstore_elem(d, w, in, m, 1);
}
static xts_mres_t m_stvehx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)out;
  return vstore_elem(d, w, in, m, 2);
}
static xts_mres_t m_stvewx(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)out;
  return vstore_elem(d, w, in, m, 4);
}

/* lvsl / lvsr: shift-permute control vectors from the EA's low 4 bits; no memory access */
static xts_mres_t m_lvsl(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  unsigned sh = (unsigned)(vea(d, w, in) & 15);
  xts_vr_t v;
  for (int i = 0; i < 16; i++) xts_vr_set_u8(&v, i, (uint8_t)(sh + (unsigned)i));
  out->vr[ml_vreg(d, w)] = v;
  return XTS_M_OK;
}
static xts_mres_t m_lvsr(const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out)
{
  (void)m;
  unsigned sh = (unsigned)(vea(d, w, in) & 15);
  xts_vr_t v;
  for (int i = 0; i < 16; i++) xts_vr_set_u8(&v, i, (uint8_t)(16 - sh + (unsigned)i));
  out->vr[ml_vreg(d, w)] = v;
  return XTS_M_OK;
}

XTS_MODEL("lvx", vload);
XTS_MODEL("lvxl", vload);
XTS_MODEL("lvebx", vload);
XTS_MODEL("lvehx", vload);
XTS_MODEL("lvewx", vload);
XTS_MODEL("lvx128", vload);
XTS_MODEL("lvxl128", vload);
XTS_MODEL("lvewx128", vload);
XTS_MODEL("stvx", vstore);
XTS_MODEL("stvxl", vstore);
XTS_MODEL("stvx128", vstore);
XTS_MODEL("stvxl128", vstore);
XTS_MODEL("stvebx", m_stvebx);
XTS_MODEL("stvehx", m_stvehx);
XTS_MODEL("stvewx", m_stvewx);
XTS_MODEL("stvewx128", m_stvewx);
XTS_MODEL("lvsl", m_lvsl);
XTS_MODEL("lvsr", m_lvsr);
