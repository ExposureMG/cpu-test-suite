/* XTS model - symbols the reference model exports to other framework modules (exec engine, semantic oracle, host tools)
 * beyond xts/model.h. Declarations only: the definitions stay in their model files (owners: see src/model/KNOWN.md).
 * Include as <model/model_priv.h>. */
#ifndef XTS_MODEL_PRIV_H
#define XTS_MODEL_PRIV_H
#include <stdint.h>

/* larx/stcx reservation (src/model/ldst.c): hidden model state, see KNOWN.md "ldst" */
void xts_model_resv_clear(void);
void xts_model_resv_get(int *valid, uint64_t *line);
void xts_model_resv_set(int valid, uint64_t line);

/* estimate accuracy check (src/model/fp_core.c, declared in fp_core.h): kind 0 recip, 1 rsqrt (2/3 exp2/log2: -1);
 * vector 0 = fres/frsqrte bounds (2^-8 / 2^-5), 1 = vrefp/vrsqrtefp (2^-12); x and est as doubles (vector lanes widened
 * exactly). 1 within the bound, 0 outside, -1 not applicable (special operand/result). */
int xf_estimate_ok(int kind, int vector, uint64_t x, uint64_t est);
#endif
