/* Private helpers shared by the load/store model files (ldst.c, ldst_vec.c, cellvmx.c). Not a public interface. */
#ifndef XTS_MODEL_LDST_PRIV_H
#define XTS_MODEL_LDST_PRIV_H
#include <stdint.h>
#include <xts/enc.h>
#include <xts/model.h>

/* effective address base: (RA|0) for the RA0 operand class */
static inline uint64_t ml_ra0(const xts_state_t *s, unsigned ra) { return ra ? s->gpr[ra & 31] : 0; }
/* MSR[SF]=0: the effective address is the low 32 bits of the sum */
static inline uint64_t ml_trunc(const xts_state_t *s, uint64_t ea) { return xm_sf(s) ? ea : (ea & 0xFFFFFFFFull); }

/* X-form (RA|0)+(RB) with the register fields taken from the descriptor operands `ia` and `ib` */
static inline uint64_t ml_ea_x_ops(const xts_insn_t *d, uint32_t w, const xts_state_t *s, int ia, int ib)
{
  unsigned ra = (unsigned)xts_field_get(d->ops[ia].field, w), rb = (unsigned)xts_field_get(d->ops[ib].field, w);
  return ml_trunc(s, ml_ra0(s, ra) + s->gpr[rb & 31]);
}

/* the quadword-addressed vector register field of operand 0 (VD/VS 5 bit or VD128/VS128 7 bit) */
static inline unsigned ml_vreg(const xts_insn_t *d, uint32_t w) { return (unsigned)xts_field_get(d->ops[0].field, w) & 127; }

/* Cell/VMX128 left/right vector loads and stores on a quadword (sh = EA & 15). All return 0 or XTS_M_DSI.
 * lvlx: bytes EA..end of the quadword go to the left (high-order) end of the register, the rest is zero.
 * lvrx: bytes quadword start..EA-1 go to the right (low-order) end of the register, the rest is zero.
 * stvlx/stvrx are the inverse (they write only those bytes of memory). */
int ml_lvlx(xts_mem_t *m, uint64_t ea, xts_vr_t *vd);
int ml_lvrx(xts_mem_t *m, uint64_t ea, xts_vr_t *vd);
int ml_stvlx(xts_mem_t *m, uint64_t ea, const xts_vr_t *vs);
int ml_stvrx(xts_mem_t *m, uint64_t ea, const xts_vr_t *vs);
#endif
