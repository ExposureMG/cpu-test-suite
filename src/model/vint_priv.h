/* Shared helpers of the vector integer model (vint.c, vperm.c). Not an interface: only those two files include it.
 * Vector element numbering is big-endian (element 0 = most significant = lowest address), as in state.h. */
#ifndef XTS_VINT_PRIV_H
#define XTS_VINT_PRIV_H
#include <xts/enc.h>
#include <xts/model.h>

#define VI_PARAMS const xts_insn_t *d, uint32_t w, const xts_state_t *in, xts_mem_t *m, xts_state_t *out
#define VI_UNUSED (void)d; (void)m

/* register fields: 5-bit AltiVec or 7-bit VMX128 */
static inline int vi_vd(uint32_t w, int x128) { return x128 ? (int)xts_field_get(XTS_F_VD128, w) : (int)XM_RD(w); }
static inline int vi_va(uint32_t w, int x128) { return x128 ? (int)xts_field_get(XTS_F_VA128, w) : (int)XM_RA(w); }
static inline int vi_vb(uint32_t w, int x128) { return x128 ? (int)xts_field_get(XTS_F_VB128, w) : (int)XM_RB(w); }

/* element access, esz = 1, 2 or 4 bytes */
static inline uint32_t vi_get(const xts_vr_t *v, int esz, int i)
{
  return esz == 1 ? xts_vr_u8(v, i) : esz == 2 ? xts_vr_u16(v, i) : v->w[i];
}
static inline void vi_set(xts_vr_t *v, int esz, int i, uint32_t x)
{
  if (esz == 1) xts_vr_set_u8(v, i, (uint8_t)x);
  else if (esz == 2) xts_vr_set_u16(v, i, (uint16_t)x);
  else v->w[i] = x;
}
static inline int64_t vi_sext(uint32_t x, int esz)
{
  return esz == 1 ? (int64_t)(int8_t)x : esz == 2 ? (int64_t)(int16_t)x : (int64_t)(int32_t)x;
}
static inline void vi_set_sat(xts_state_t *out) { out->vscr.w[3] |= XTS_VSCR_SAT; }
#endif
