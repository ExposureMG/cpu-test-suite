/* Shared helpers of vfp.c / vfp_est.c (owner: mfp). */
#ifndef XTS_MODEL_VFP_H
#define XTS_MODEL_VFP_H
#include "fp_core.h"

typedef struct vops { int vd, va, vb, vc, uimm; } vops_t;   /* register numbers / UIMM, -1 if the form has none */
vops_t xv_ops(const xts_insn_t *d, uint32_t w);

static inline int xv_njmode(const xts_state_t *s) { return (s->vscr.w[3] & XTS_VSCR_NJ) != 0; }
/* VSCR[NJ]=1: a denormal becomes a zero of the same sign (inputs and results) */
static inline uint32_t xv_nj(uint32_t v, int nj) { return nj && xs_isdenorm(v) ? v & XS_SIGN : v; }
/* vector ops: round to nearest even, flags ignored */
static inline void xv_sf_begin(void) { softfloat_roundingMode = softfloat_round_near_even; softfloat_exceptionFlags = 0; }
#endif
