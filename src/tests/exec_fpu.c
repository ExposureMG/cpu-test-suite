/* exec.fpu - scalar floating point: every unit-FPU instruction the hardware executes plus the FPSCR instructions (mffs, mtfsf,
 * mtfsfi, mtfsb0, mtfsb1, mcrfs), random operands, CPU vs reference model. Split out of exec.ppc so the rounding-mode /
 * exception-enable axes (fpscr_rn, fpscr_en) are only swept here.
 *
 * Operand values: FP64 list + widened FP32 list + structured / random draws (xe_fp_value). 30 % of the arithmetic cases also
 * start with random FPSCR status bits (sticky exceptions, FR/FI/FPRF, FX) so the "FX only on 0 -> 1" and sticky behaviour is
 * checked; RN and the enable bits are left to the axes (xts_case_apply_env re-applies them after the random bits).
 * The FPSCR instructions get a fully random status image and exhaustive immediates: mtfsb0/1 every bit x Rc, mcrfs every
 * CRFD x CRFS, mtfsfi every CRFD x IMM x Rc, mtfsf the 16 single-field / complement masks plus random masks.
 * (mffs' descriptor lists its destination as a GPR field named RD; it is an FPR on hardware. The register is just a number
 * here, the generator treats it as a GPR index and the check reads FPR out.) */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "exec_common.h"

enum { F_GEN = 0, F_BIT, F_MCRFS, F_MTFSFI, F_MTFSF };

static int is_mnem(const xts_insn_t *d, const char *m) { return !strcmp(d->mnem, m); }

static int is_fpu(const xts_insn_t *d)
{
  if (d->unit == XTS_UNIT_FPU) return 1;
  return is_mnem(d, "mtfsb0x") || is_mnem(d, "mtfsb1x") || is_mnem(d, "mcrfs") || is_mnem(d, "mffsx") ||
         is_mnem(d, "mtfsfix") || is_mnem(d, "mtfsfx");
}

static int fpu_enumerate(xts_caselist_t *cl)
{
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    char sfx[24];
    if (d->isa != XTS_ISA_PPC || d->priv != XTS_PRIV_USER || !is_fpu(d)) continue;
    if (d->hw != XTS_HW_EXECUTES && d->hw != XTS_HW_TRAPS) continue;
    int kind = F_GEN, n = XE_DRAWS;
    if (is_mnem(d, "mtfsb0x") || is_mnem(d, "mtfsb1x")) { kind = F_BIT; n = 64; }
    else if (is_mnem(d, "mcrfs")) { kind = F_MCRFS; n = 64; }
    else if (is_mnem(d, "mtfsfix")) { kind = F_MTFSFI; n = 256; }
    else if (is_mnem(d, "mtfsfx")) { kind = F_MTFSF; n = 64; }
    unsigned var[8];
    int nv = xe_flag_variants(d, XTS_RC, var);
    for (int k = 0; k < n; k++) {
      unsigned fl = var[k % nv];
      if (kind == F_BIT) fl = (k >= 32 && nv > 1) ? XTS_RC : 0;
      xe_flag_suffix(fl, sfx, sizeof sfx);
      xts_case_t *c = xts_case_addf(cl, "%s%s#%d", d->mnem, sfx, k);
      if (!c) return -1;
      c->insn = id;
      c->flags = fl;
      c->arg[0] = k;
      c->arg[3] = kind;
    }
  }
  return 0;
}

#define FPSCR_STATUS_MASK 0xFFFFFF00u    /* everything above the enables / NI / RN byte (axes own the low byte) */

static int fpu_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int k = (int)c->arg[0], kind = (int)c->arg[3];
  int64_t ops[XTS_MAX_OPS], ov[XTS_MAX_OPS];
  xe_gen_t g;
  xts_case_init(&x);
  xe_gen_defaults(d, &g);
  for (int i = 0; i < XTS_MAX_OPS; i++) ov[i] = XE_NOV;
  switch (kind) {
  case F_BIT: ov[0] = k % 32; break;
  case F_MCRFS: ov[0] = k / 8; ov[1] = k % 8; break;
  case F_MTFSFI: ov[0] = (k >> 1) & 7; ov[1] = (k >> 4) & 15; break;
  case F_MTFSF:
    if (k < 8) ov[0] = 1 << k;
    else if (k < 16) ov[0] = 0xFF & ~(1 << (k - 8));
    else ov[0] = xe_edge(r, 0, 255);
    break;
  default: break;
  }
  if (xe_gen_ops(d, r, &x, ops, ov, &g) < 0) return -1;
  xe_noise(r, &x, d);
  if (kind != F_GEN || xe_pct(r, 30)) {
    /* random status bits; sparse for arithmetic, dense for the FPSCR instructions */
    uint32_t m = (uint32_t)xts_rng_next(r), m2 = (uint32_t)xts_rng_next(r);
    uint32_t bits = kind == F_GEN ? (m & m2 & (uint32_t)xts_rng_next(r)) : (m & (m2 | (uint32_t)xts_rng_next(r)));
    x.in.fpscr = ((uint32_t)x.in.fpscr & ~FPSCR_STATUS_MASK) | (bits & FPSCR_STATUS_MASK);
    xts_case_apply_env(&x);
  }
  if (kind == F_MTFSF) {
    int fb = (int)ops[1];
    uint64_t v = xts_rng_next(r);
    if (xe_pct(r, 30)) v = xe_fp_value(r);
    xts_set_fpr(&x, fb, v);
  }
  uint32_t w = xe_encode(d, c->flags, ops);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  xts_semantic_check(ctx, &x, w, NULL);
  return 0;
}

XTS_FAMILY(exec_fpu, .kind = "semantic", .axes = "sf,fpscr_rn,fpscr_en", .enumerate = fpu_enumerate, .run = fpu_run,
           .block_cases = 48, .desc = "scalar FP + FPSCR instructions vs the reference model (rounding mode / exception enables swept)");
