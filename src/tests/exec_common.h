/* exec_common.h - private helpers shared by the exec.* families (exec_ppc.c, exec_fpu.c, exec_altivec.c, exec_vmx128.c,
 * exec_illegal.c, vmx128_regmap.c). Everything is static (no symbol leaks between families).
 *
 * Operand generation is a port of the old plan builder (isa/hwtest/host/plans.py: pick_regs, imm_domain, build_case):
 *   - distinct registers per class (aliasing is added at a configurable rate, the old plan never aliased),
 *   - memory instructions: RA / RA0 = pointer to scratch+128 (never r0 as base), RB = small offset (MEM_OFFSETS), D-form
 *     displacements in -64..64 so the EA stays inside the 512-byte scratch,
 *   - RA0 = 0 ("RA|0") for 20 % of the non-memory RA0 operands,
 *   - VMX128 cases draw 70 % of their registers from v32..v127, VC128 from v0..v7,
 *   - value sets INT64 / FP64 / FP32 / INT32 / XER_VALS as in the old plan, extended with a few saturation / conversion
 *     boundaries and random draws.
 * Everything is driven by the descriptor table (operand field and class), not by mnemonic lists, except where the
 * architecture needs a special domain (documented where it happens). */
#ifndef XE_COMMON_H
#define XE_COMMON_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <exec/exec_priv.h>   /* xts_exec_mem_guard */
#include <xts/xts.h>

#define XE_S static __attribute__((unused))
#define XE_DRAWS 24            /* default cases per instruction */
#define XE_WINDOW 128          /* memory pointers point at scratch+128 */
#define XE_NOV INT64_MIN       /* "no override" marker for xe_gen_ops */

/* ---------------------------------------------------------------------------------------------------- value sets */
XE_S const uint64_t xe_int64[] = {
  0, 1, 2, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x00000000FFFFFFFFull,
  0x0000000080000000ull, 0x000000007FFFFFFFull, 31, 32, 33, 63, 64, 0x123456789ABCDEF0ull, 0xFFFFFFFF00000000ull,
  0x00000000FFFF8000ull, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
};
XE_S const uint32_t xe_int32[] = {
  0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF,
  0x01010101, 0x80808080, 0x7F7F7F7F, 0xFF00FF00, 0x00FF00FF, 0xFFFF0000,
  /* saturation edges (packs, adds, subs, multiplies) */
  0x7FFF7FFF, 0x80008000, 0x7FFF8000, 0x80017FFF, 0xFF80FF80, 0x007F007F, 0x00800080, 0x7F807F80, 0x8001FFFF, 0xFFFF0001,
};
XE_S const uint32_t xe_fp32[] = {
  0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3FC00000, 0xC0200000, 0x3DCCCCCD, 0x42C80000, 0x7F7FC99E,
  0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0x00000001, 0x007FFFFF, 0x00800000, 0x4B000000,
  0x4B800000, 0x477FE000, 0xBF000000,
  /* denormals of both signs (NJ), conversion boundaries (2^31, -2^31, 2^32), quiet/signalling NaN payloads */
  0x80000001, 0x807FFFFF, 0x00400000, 0x4F000000, 0xCF000000, 0x4F800000, 0xFFC00001, 0xFF800001, 0x40400000, 0x3F000000,
};
XE_S const uint64_t xe_fp64[] = {
  0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x3FF8000000000000ull,
  0x4004000000000000ull, 0x3FB999999999999Aull, 0x7E37E43C8800759Cull, 0x01A56E1FC2F8F359ull, 0x7FF0000000000000ull,
  0xFFF0000000000000ull, 0x7FF8000000000000ull, 0x7FF0000000000001ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull,
  0x0010000000000000ull, 0x4330000000000000ull, 0x4340000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0xC0FE240C9FBE76C9ull,
  /* conversion boundaries (fctid / fctiw), sqrt / estimate arguments, rounding-sensitive values */
  0x43E0000000000000ull, 0xC3E0000000000000ull, 0x41E0000000000000ull, 0xC1E0000000200000ull, 0x41F0000000000000ull,
  0x3FE0000000000000ull, 0x4008000000000000ull, 0xC010000000000000ull, 0x3FF0000000000001ull, 0x4170000010000000ull,
  0x800FFFFFFFFFFFFFull, 0xFFF8000000000000ull, 0xFFF0000000000001ull,
};
XE_S const uint64_t xe_xer[] = { 0, 0x20000000, 0x80000000, 0x60000000, 0xE0000000, 0x40000000 };
XE_S const int xe_memoff[] = { 0, 1, 2, 3, 4, 5, 7, 8, 12, 15, 16, 17, 31, 32, 64, 96, 127, 128 };
#define XE_N(a) ((int)(sizeof(a) / sizeof((a)[0])))

XE_S int xe_pct(xts_rng_t *r, int pct) { return (int)xts_rng_below(r, 100) < pct; }
XE_S uint64_t xe_pick64(xts_rng_t *r, const uint64_t *a, int n) { return a[xts_rng_below(r, (uint32_t)n)]; }
XE_S uint32_t xe_pick32(xts_rng_t *r, const uint32_t *a, int n) { return a[xts_rng_below(r, (uint32_t)n)]; }

XE_S uint64_t xe_gpr_value(xts_rng_t *r)
{
  if (xts_rng_below(r, 4) == 0) return xts_rng_next(r);
  return xe_pick64(r, xe_int64, XE_N(xe_int64));
}

XE_S uint64_t xe_widen32(uint32_t f)       /* single -> double bit pattern (what an FPR holds after lfs) */
{
  union { uint32_t u; float f; } a;
  union { uint64_t u; double d; } b;
  a.u = f;
  b.d = (double)a.f;
  return b.u;
}

XE_S uint64_t xe_fp_value(xts_rng_t *r)
{
  uint32_t m = xts_rng_below(r, 20);
  if (m < 9) return xe_pick64(r, xe_fp64, XE_N(xe_fp64));
  if (m < 13) return xe_widen32(xe_pick32(r, xe_fp32, XE_N(xe_fp32)));
  if (m < 15) return xts_rng_next(r);
  /* draws in sequence (sign, exponent, mantissa): one expression would leave their order to the compiler */
  uint64_t s = xts_rng_below(r, 2), e;
  if (m < 18) e = (uint64_t)(1023 + (int)xts_rng_below(r, 21) - 10);   /* exponent within +-10 of 1.0 */
  else e = xts_rng_below(r, 2) ? 0x7FF : 0;                           /* denormal / max-exponent */
  return (s << 63) | (e << 52) | (xts_rng_next(r) & 0xFFFFFFFFFFFFFull);
}

XE_S int xe_is_float_vec(const xts_insn_t *d)
{
  static const char *const t[] = { "fp", "vcfsx", "vcfux", "vcsxwfp", "vcuxwfp", "vrefp", "vrsqrtefp", "vexptefp", "vlogefp",
                                   "vrfi", "vmaddfp", "vnmsub", "vperm", "vsel", "vmsum3", "vmsum4", "vmulfp", "vpkd3d",
                                   "vupkd3d", "vctsxs", "vctuxs", "vcfpsxws", "vcfpuxws", "vmaxfp", "vminfp", NULL };
  for (int i = 0; t[i]; i++)
    if (strstr(d->mnem, t[i])) return 1;
  return 0;
}

XE_S xts_vr_t xe_vec_value(xts_rng_t *r, int floaty)
{
  uint32_t w[4];
  uint32_t m = xts_rng_below(r, 100);
  if (floaty) {
    if (m < 65) for (int i = 0; i < 4; i++) w[i] = xe_pick32(r, xe_fp32, XE_N(xe_fp32));
    else if (m < 78) { uint32_t s = xe_pick32(r, xe_fp32, XE_N(xe_fp32)); for (int i = 0; i < 4; i++) w[i] = s; }
    else if (m < 90) for (int i = 0; i < 4; i++) w[i] = xts_rng_below(r, 2) ? xe_pick32(r, xe_fp32, XE_N(xe_fp32)) : xe_pick32(r, xe_int32, XE_N(xe_int32));
    else for (int i = 0; i < 4; i++) w[i] = (uint32_t)xts_rng_next(r);
  } else {
    if (m < 40) for (int i = 0; i < 4; i++) w[i] = xe_pick32(r, xe_int32, XE_N(xe_int32));
    else if (m < 58) { uint32_t s = xe_pick32(r, xe_int32, XE_N(xe_int32)); for (int i = 0; i < 4; i++) w[i] = s; }
    else if (m < 72) {                                  /* byte splat */
      static const uint8_t b[] = { 0, 1, 0x7F, 0x80, 0xFF, 0x55, 0xAA };
      uint32_t v = xts_rng_below(r, 4) == 0 ? (uint32_t)(xts_rng_next(r) & 0xFF) : b[xts_rng_below(r, 7)];
      for (int i = 0; i < 4; i++) w[i] = v * 0x01010101u;
    } else if (m < 84) {                                /* halfword splat / mix */
      static const uint16_t h[] = { 0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x00FF, 0xFF00 };
      for (int i = 0; i < 4; i++) { uint32_t hi = h[xts_rng_below(r, 7)]; w[i] = (hi << 16) | h[xts_rng_below(r, 7)]; }
    } else for (int i = 0; i < 4; i++) w[i] = (uint32_t)xts_rng_next(r);
  }
  return xts_vr4(w[0], w[1], w[2], w[3]);
}

/* ---------------------------------------------------------------------------------------------------- immediates */
/* Small enumerable immediate domains: returns 1 and [lo, hi] when operand i is an immediate whose whole range is worth
 * sweeping exhaustively (<= 256 values). Legacy AltiVec splats use their architected 4/3/2-bit element index. */
XE_S int xe_imm_domain(const xts_insn_t *d, int i, int64_t *lo, int64_t *hi)
{
  const xts_operand_t *o = &d->ops[i];
  int legacy = d->isa != XTS_ISA_VMX128;
  switch (o->field) {
  case XTS_F_UIMM5:
    *lo = 0; *hi = 31;
    if (legacy && !strncmp(d->mnem, "vspltb", 6)) *hi = 15;
    else if (legacy && !strncmp(d->mnem, "vsplth", 6)) *hi = 7;
    else if (legacy && !strncmp(d->mnem, "vspltw", 6)) *hi = 3;
    return 1;
  case XTS_F_SIMM5: *lo = -16; *hi = 15; return 1;
  case XTS_F_SHB: *lo = 0; *hi = 15; return 1;
  case XTS_F_VD3D0: *lo = 0; *hi = 7; return 1;
  case XTS_F_VD3D1: *lo = 0; *hi = 3; return 1;
  case XTS_F_VD3D2: *lo = 0; *hi = 3; return 1;
  case XTS_F_VPERM128: *lo = 0; *hi = 255; return 1;
  case XTS_F_STRM: *lo = 0; *hi = 3; return 1;
  default: return 0;
  }
}

/* generic random immediate for operand i (edge-biased) */
XE_S int64_t xe_edge(xts_rng_t *r, int64_t lo, int64_t hi)
{
  if (xe_pct(r, 25)) {
    int64_t e[] = { lo, hi, lo + 1, hi - 1, 0, (lo + hi) / 2 };
    int64_t v = e[xts_rng_below(r, 6)];
    if (v >= lo && v <= hi) return v;
  }
  return xts_rng_range(r, lo, hi);
}

XE_S int64_t xe_imm(const xts_insn_t *d, int i, xts_rng_t *r)
{
  const xts_operand_t *o = &d->ops[i];
  int64_t lo, hi;
  if (xe_imm_domain(d, i, &lo, &hi)) return xts_rng_range(r, lo, hi);
  switch (o->field) {
  case XTS_F_UIMM16: return xe_edge(r, 0, 0xFFFF);
  case XTS_F_SIMM16: return xe_edge(r, -0x8000, 0x7FFF);
  case XTS_F_D16: return xts_rng_range(r, -64, 64);
  case XTS_F_DS14: return xts_rng_range(r, -16, 16) * 4;
  case XTS_F_TO: return xts_rng_range(r, 0, 31);
  case XTS_F_L: return xts_rng_range(r, 0, 1);
  case XTS_F_CRFD: case XTS_F_CRFS: return xts_rng_range(r, 0, 7);
  case XTS_F_CRBD: case XTS_F_CRBA: case XTS_F_CRBB: case XTS_F_FPSCRD: case XTS_F_BI: return xts_rng_range(r, 0, 31);
  case XTS_F_SH5: case XTS_F_MB5: case XTS_F_ME5: return xe_edge(r, 0, 31);
  case XTS_F_SH6: case XTS_F_MB6: case XTS_F_ME6: return xe_edge(r, 0, 63);
  case XTS_F_NB: return xts_rng_range(r, 0, 31);
  case XTS_F_CRM:
    if (!strcmp(d->mnem, "mfocrf") || !strcmp(d->mnem, "mtocrf")) return 1 << xts_rng_below(r, 8);   /* one field: other forms undefined */
    return xe_edge(r, 0, 255);
  case XTS_F_FM: return xe_edge(r, 0, 255);
  case XTS_F_IMM: return xts_rng_range(r, 0, 15);
  case XTS_F_SR: return xts_rng_range(r, 0, 15);
  case XTS_F_BO: return xts_rng_range(r, 0, 31);
  case XTS_F_SPR: return 1;
  case XTS_F_TBR: return 268;
  case XTS_F_LEV: return 0;
  case XTS_F_BD: case XTS_F_LI: return 8;
  default: return 0;
  }
}

/* ---------------------------------------------------------------------------------------------------- operand generation */
typedef struct xe_gen {
  int alias_pct;            /* chance that a data register reuses an earlier register of its class (0 for memory insns) */
  int vr_hi_pct;            /* VMX128: chance to draw from v32..v127 */
  int ra0_zero_pct;         /* non-memory RA0: chance of the literal 0 */
  int mem_ra0_zero_pct;     /* X-form memory with RA0: chance of "0, RB=pointer" */
  int floaty;
} xe_gen_t;

XE_S void xe_gen_defaults(const xts_insn_t *d, xe_gen_t *g)
{
  memset(g, 0, sizeof *g);
  int mem = d->unit == XTS_UNIT_MEMORY;
  g->alias_pct = mem ? 0 : 25;
  g->vr_hi_pct = 70;
  g->ra0_zero_pct = 20;
  g->mem_ra0_zero_pct = 15;
  g->floaty = xe_is_float_vec(d);
}

XE_S int xe_new_reg(xts_rng_t *r, uint32_t *used, int n, int hi_pct, int allow0)
{
  for (int tries = 0; tries < 1000; tries++) {
    int reg;
    if (n > 32 && xe_pct(r, hi_pct)) reg = 32 + (int)xts_rng_below(r, (uint32_t)(n - 32));
    else reg = (int)xts_rng_below(r, n > 32 ? 32 : (uint32_t)n);
    if (!allow0 && reg == 0) continue;
    if (used[reg >> 5] & (1u << (reg & 31))) continue;
    used[reg >> 5] |= 1u << (reg & 31);
    return reg;
  }
  return -1;
}

/* Choose every operand of `d` and put the inputs into x (registers, pointers, values). ops[] receives the operand values
 * in descriptor order. ov (may be NULL): per-operand override, XE_NOV = generate; overridden register operands are used
 * as given and the caller sets their state. Returns 0, or -1 if registers ran out. */
XE_S int xe_gen_ops(const xts_insn_t *d, xts_rng_t *r, xts_exec_t *x, int64_t *ops, const int64_t *ov, const xe_gen_t *g)
{
  uint32_t used[3][4] = { { XTS_RESERVED_GPRS, 0, 0, 0 }, { 0 }, { 0 } };
  int chosen[3][XTS_MAX_OPS], nch[3] = { 0, 0, 0 };
  const int mem = d->unit == XTS_UNIT_MEMORY;
  int has_ra0 = 0, has_rb = 0, ra0_zero = 0;
  for (int i = 0; i < d->nops; i++) {
    if (d->ops[i].field == XTS_F_RA0) has_ra0 = 1;
    if (d->ops[i].field == XTS_F_RB) has_rb = 1;
  }
  if (mem && has_ra0 && has_rb && xe_pct(r, g->mem_ra0_zero_pct)) ra0_zero = 1;
  for (int i = 0; i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    int ci = -1, nreg = 0;
    switch (o->cls) {
    case XTS_OC_GPR: case XTS_OC_GPR0: ci = 0; nreg = 32; break;
    case XTS_OC_FPR: ci = 1; nreg = 32; break;
    case XTS_OC_VR: ci = 2; nreg = 32; break;
    case XTS_OC_VR128: ci = 2; nreg = 128; break;
    case XTS_OC_VRC128: ci = 2; nreg = 8; break;
    default: break;
    }
    if (ov && ov[i] != XE_NOV) {
      ops[i] = ov[i];
      if (ci >= 0 && ov[i] >= 0 && ov[i] < 128) used[ci][ov[i] >> 5] |= 1u << (ov[i] & 31);
      continue;
    }
    if (ci < 0) { ops[i] = xe_imm(d, i, r); continue; }
    /* tied VMX128 source: same register as the destination, nothing else to set */
    if ((o->role & XTS_ROLE_TIED) && (o->field == XTS_F_VS128)) {
      int j;
      for (j = 0; j < i; j++)
        if (d->ops[j].field == XTS_F_VD128) break;
      if (j < i) { ops[i] = ops[j]; continue; }
    }
    if (ci == 0) {
      int f = o->field;
      if (mem && (f == XTS_F_RA || f == XTS_F_RA0)) {
        if (f == XTS_F_RA0 && ra0_zero) { ops[i] = 0; continue; }
        int reg = xe_new_reg(r, used[0], 32, 0, 0);
        if (reg < 0) return -1;
        ops[i] = reg;
        xts_set_ptr(x, reg, XE_WINDOW);
        continue;
      }
      if (mem && f == XTS_F_RB) {
        int reg = xe_new_reg(r, used[0], 32, 0, 1);
        if (reg < 0) return -1;
        ops[i] = reg;
        if (ra0_zero) xts_set_ptr(x, reg, XE_WINDOW + xe_memoff[xts_rng_below(r, 8)]);
        else xts_set_gpr(x, reg, (uint64_t)xe_memoff[xts_rng_below(r, XE_N(xe_memoff))]);
        continue;
      }
      if (f == XTS_F_RA0 && !mem && xe_pct(r, g->ra0_zero_pct)) { ops[i] = 0; continue; }
      int reg;
      if (nch[0] && xe_pct(r, g->alias_pct)) reg = chosen[0][xts_rng_below(r, (uint32_t)nch[0])];
      else {
        reg = xe_new_reg(r, used[0], 32, 0, 1);
        if (reg < 0) return -1;
        chosen[0][nch[0]++] = reg;
        xts_set_gpr(x, reg, xe_gpr_value(r));
      }
      ops[i] = reg;
      continue;
    }
    int reg;
    reg = -1;
    if (nch[ci] && xe_pct(r, g->alias_pct)) {
      reg = chosen[ci][xts_rng_below(r, (uint32_t)nch[ci])];
      if (reg >= nreg) reg = -1;                         /* VC128 can only name v0..v7 */
    }
    if (reg < 0) {
      reg = xe_new_reg(r, used[ci], nreg, nreg > 32 ? g->vr_hi_pct : 0, 1);
      if (reg < 0) return -1;
      chosen[ci][nch[ci]++] = reg;
      if (ci == 1) xts_set_fpr(x, reg, xe_fp_value(r));
      else xts_set_vr(x, reg, xe_vec_value(r, g->floaty));
    }
    ops[i] = reg;
  }
  return 0;
}

/* CR / XER / LR / CTR noise, memory contents and VSCR[SAT] for the instruction class */
XE_S void xe_noise(xts_rng_t *r, xts_exec_t *x, const xts_insn_t *d)
{
  if (xe_pct(r, 50)) xts_set_cr(x, (uint32_t)xts_rng_next(r));
  if (xe_pct(r, 50)) xts_set_xer(x, xe_xer[xts_rng_below(r, XE_N(xe_xer))]);
  if (xe_pct(r, 15)) xts_set_lr(x, xts_rng_next(r));
  if (xe_pct(r, 15)) xts_set_ctr(x, xts_rng_next(r));
  if (d->unit == XTS_UNIT_VECTOR && xe_pct(r, 30)) x->in.vscr.w[3] |= XTS_VSCR_SAT;
  if (d->unit == XTS_UNIT_MEMORY) {
    uint32_t m = xts_rng_below(r, 10);
    uint8_t buf[256];
    if (m < 5) return;                      /* keep the baseline pattern */
    if (m < 8) {
      for (int i = 0; i < 256; i++) buf[i] = (uint8_t)xts_rng_next(r);
    } else {                                /* FP-looking contents: doubles and singles back to back */
      for (int i = 0; i < 256; i += 8) {
        if (xe_pct(r, 50)) xts_be_store(buf + i, 8, xe_fp_value(r));
        else { xts_be_store(buf + i, 4, xe_pick32(r, xe_fp32, XE_N(xe_fp32))); xts_be_store(buf + i + 4, 4, xe_pick32(r, xe_fp32, XE_N(xe_fp32))); }
      }
    }
    xts_set_mem(x, XE_WINDOW - 64, buf, 256);
  }
}

/* ---------------------------------------------------------------------------------------------------- encoding / flags */
XE_S uint32_t xe_encode(const xts_insn_t *d, unsigned flags, const int64_t *ops)
{
  uint32_t w = 0;
  int bad;
  if (xts_encode(d->index, flags, ops, &w, &bad)) return 0;
  return w;
}

/* flag variants of an instruction (subsets of d->flags & mask), returns the count (>= 1) */
XE_S int xe_flag_variants(const xts_insn_t *d, unsigned mask, unsigned *out)
{
  unsigned fl = d->flags & mask;
  int n = 0;
  for (unsigned v = 0; v <= fl; v++)
    if (!(v & ~fl)) out[n++] = v;
  return n;
}
XE_S const char *xe_flag_suffix(unsigned fl, char *buf, int size)
{
  snprintf(buf, (size_t)size, "%s%s%s%s%s%s", (fl & XTS_OE) ? ".oe" : "", (fl & XTS_RC) ? ".rc" : "", (fl & XTS_LK) ? ".lk" : "",
           (fl & XTS_AA) ? ".aa" : "", (fl & XTS_T) ? ".t" : "", (fl & XTS_A) ? ".all" : "");
  return buf;
}

/* ---------------------------------------------------------------------------------------------------- records */
XE_S void xe_emit_json(xts_ctx_t *ctx, int status, const char *mnem, uint32_t word, int has_word, xts_json_t *j)
{
  xj_finish(j);
  xts_record_t rec;
  xts_record_init(&rec);
  rec.mnem = mnem;
  rec.word = word;
  rec.has_word = has_word;
  rec.status = status;
  rec.detail = j->buf;
  rec.detail_truncated = j->truncated;
  xts_emit(ctx, &rec);
}

/* one-line record: {"why":"..."} (skip / info / fail with a reason) */
XE_S void xe_emit_why(xts_ctx_t *ctx, int status, const char *mnem, uint32_t word, int has_word, const char *why)
{
  char det[320];
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  xj_kv_str(&j, "why", why);
  xe_emit_json(ctx, status, mnem, word, has_word, &j);
}

XE_S void xe_put_trap(xts_json_t *j, const char *key, int trapped, uint32_t vector, uint32_t cause, uint64_t srr0)
{
  xj_key(j, key);
  xj_obj(j);
  xj_kv_bool(j, "trapped", trapped);
  if (trapped) {
    xj_kv_hex(j, "vector", vector, 4);
    xj_kv_hex(j, "cause", cause, 2);
    if (srr0) xj_kv_hex(j, "srr0", srr0, 8);
  }
  xj_end(j);
}

XE_S void xe_put_inputs(xts_json_t *j, const xts_insn_t *d, uint32_t word, const xts_state_t *in)
{
  xj_key(j, "in");
  xj_obj(j);
  for (int i = 0; d && i < d->nops; i++) {
    const xts_operand_t *o = &d->ops[i];
    int64_t v = xts_field_get(o->field, word);
    char k[24];
    if (o->cls == XTS_OC_GPR || (o->cls == XTS_OC_GPR0 && v)) {
      snprintf(k, sizeof k, "%s=r%d", o->name, (int)v);
      xj_kv_hex(j, k, in->gpr[v & 31], 16);
    } else if (o->cls == XTS_OC_FPR) {
      snprintf(k, sizeof k, "%s=f%d", o->name, (int)v);
      xj_kv_hex(j, k, in->fpr[v & 31], 16);
    } else if (o->cls == XTS_OC_VR || o->cls == XTS_OC_VR128 || o->cls == XTS_OC_VRC128) {
      snprintf(k, sizeof k, "%s=v%d", o->name, (int)v);
      char s[40];
      const xts_vr_t *rr = &in->vr[v & 127];
      snprintf(s, sizeof s, "%08X%08X%08X%08X", rr->w[0], rr->w[1], rr->w[2], rr->w[3]);
      xj_kv_str(j, k, s);
    } else {
      xj_kv_int(j, o->name, v);
    }
  }
  xj_kv_hex(j, "cr", (uint32_t)in->cr, 8);
  xj_kv_hex(j, "xer", in->xer, 16);
  xj_kv_hex(j, "lr", in->lr, 16);
  xj_kv_hex(j, "ctr", in->ctr, 16);
  xj_kv_hex(j, "fpscr", (uint32_t)in->fpscr, 8);
  xj_kv_hex(j, "vscr", in->vscr.w[3], 8);
  xj_kv_int(j, "sf", (int)(in->mode & XTS_MODE_SF));
  xj_end(j);
}

XE_S void xe_put_diffs(xts_json_t *j, const char *key, const xts_state_diff_t *d, int n, int shown, const char *a_name,
                       const char *b_name)
{
  xj_key(j, key);
  xj_arr(j);
  for (int i = 0; i < shown && i < n; i++) {
    xj_obj(j);
    xj_kv_str(j, "what", d[i].what);
    int wide = !strncmp(d[i].what, "vr", 2) || !strncmp(d[i].what, "mem", 3);
    char s[40];
    if (wide) snprintf(s, sizeof s, "%016llX%016llX", (unsigned long long)d[i].a[0], (unsigned long long)d[i].a[1]);
    else snprintf(s, sizeof s, "%016llX", (unsigned long long)d[i].a[0]);
    xj_kv_str(j, a_name, s);
    if (wide) snprintf(s, sizeof s, "%016llX%016llX", (unsigned long long)d[i].b[0], (unsigned long long)d[i].b[1]);
    else snprintf(s, sizeof s, "%016llX", (unsigned long long)d[i].b[0]);
    xj_kv_str(j, b_name, s);
    xj_end(j);
  }
  xj_end(j);
  if (n > shown) xj_kv_int(j, "more_diffs", n - shown);
}

/* ---------------------------------------------------------------------------------------------------- kernel helpers */

/* Run kernel k on the CPU and check it against the model for the single instruction `word` it contains at word index
 * `pos` (the rest of the kernel is a wrapper that leaves the architected state alone). Like xts_semantic_check, but a
 * trap inside the kernel counts when it is at the instruction. One record. */
XE_S int xe_check_kernel(xts_ctx_t *ctx, xts_exec_t *x, xts_kbuf_t *k, uint32_t word, int pos, const char *note)
{
  static xts_state_t exp;
  static uint8_t mem_exp[XTS_SCRATCH_BYTES];
  static char det[6144];
  static xts_state_diff_t diffs[64];
  int id = xts_decode1(word);
  const xts_insn_t *d = xts_insn(id);
  xts_mem_t m;
  xts_mem_init(&m);
  memcpy(mem_exp, x->mem_in, XTS_SCRATCH_BYTES);
  xts_mem_add(&m, xts_scratch_addr(), mem_exp, XTS_SCRATCH_BYTES);
  xts_mres_t mr = xts_model_exec(NULL, word, &x->in, &m, &exp);
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  if (note) xj_kv_str(&j, "note", note);
  xj_kv_str(&j, "model", xts_mres_name(mr));
  xj_kv_str(&j, "via", "kernel");
  char guard[200];
  if (xts_exec_mem_guard(id, word, &x->in, mr, m.fault_ea, guard, sizeof guard)) {   /* a DSI would crash the console */
    xj_kv_str(&j, "why", guard);
    xe_put_inputs(&j, d, word, &x->in);
    xe_emit_json(ctx, XTS_ST_SKIP, d ? d->mnem : "", word, 1, &j);
    return XTS_ST_SKIP;
  }
  int st = xts_exec_kernel(x, k);
  int status;
  if (st == XTS_EXEC_NOTRUN || st == XTS_EXEC_BADKERNEL) {
    status = XTS_ST_FAIL;
    xj_kv_str(&j, "exec_error", x->err);
  } else if (st == XTS_EXEC_DRYRUN) {
    status = XTS_ST_SKIP;
    xj_kv_str(&j, "why", "host dry run: kernel validated, not executed");
  } else {
    uint32_t ev = 0, ec = 0;
    int et = 0;
    switch (mr) {
    case XTS_M_TRAP: et = 1; ev = 0x700; ec = XTS_CAUSE_TRAP; break;
    case XTS_M_ILLEGAL: et = 1; ev = 0x700; ec = XTS_CAUSE_ILLEGAL; break;
    case XTS_M_PRIVILEGED: et = 1; ev = 0x700; ec = XTS_CAUSE_PRIV; break;
    case XTS_M_ALIGNMENT: et = 1; ev = 0x600; break;
    default: break;
    }
    xe_put_trap(&j, "cpu", x->trap.trapped, x->trap.vector, x->trap.cause, x->trap.srr0);
    if (mr == XTS_M_UNIMPLEMENTED || mr == XTS_M_DSI) {
      status = XTS_ST_SKIP;
      int n = xts_state_compare(&x->in, &x->out, diffs, 64);
      int nm = xts_mem_compare(x->mem_in, x->mem_out, XTS_SCRATCH_BYTES, diffs + (n < 64 ? n : 64), 64 - (n < 64 ? n : 64));
      xe_put_inputs(&j, d, word, &x->in);
      xe_put_diffs(&j, "changes", diffs, n + nm, 24, "in", "cpu");
    } else {
      int bad = 0;
      xe_put_trap(&j, "expected", et, ev, ec, 0);
      if (et != x->trap.trapped || (et && (ev != x->trap.vector || ec != x->trap.cause))) bad = 1;
      if (x->trap.trapped && (x->trap.where != 1 || x->trap.offset != (uint32_t)pos * 4)) {
        bad = 1;
        xj_kv_str(&j, "srr0_problem", "trap not at the instruction under test");
        xj_kv_uint(&j, "trap_offset", x->trap.offset);
      }
      int n = xts_state_compare(&exp, &x->out, diffs, 64);
      int nm = xts_mem_compare(mem_exp, x->mem_out, XTS_SCRATCH_BYTES, diffs + (n < 64 ? n : 64), 64 - (n < 64 ? n : 64));
      if (n + nm) bad = 1;
      status = bad ? XTS_ST_FAIL : XTS_ST_PASS;
      if (bad) {
        xe_put_inputs(&j, d, word, &x->in);
        xe_put_diffs(&j, "diffs", diffs, n + nm, 24, "model", "cpu");
      }
    }
  }
  xe_emit_json(ctx, status, d ? d->mnem : "", word, 1, &j);
  return status;
}

/* "expected state" for oracles that do not use the model: the input as the CPU holds it after the slot loaded it */
XE_S void xe_expect_base(const xts_exec_t *x, xts_state_t *exp)
{
  *exp = x->in;
  xm_state_loaded(exp);
}
/* ---------------------------------------------------------------------------------------------------- vector families
 * Shared by exec.altivec and exec.vmx128. Cases per instruction = max(XE_DRAWS, combos * flag variants) where combos is the
 * product of the small immediate domains (vsplt*, vsldoi, vcfsx, vpkd3d128, vrlimi128, vpermwi128, ...): case k uses
 * immediate combination k % combos and flag variant (k / combos) % nvariants, so every (immediate, flag) pair occurs. */
XE_S int xe_imm_combos(const xts_insn_t *d, int *idx, int64_t *lo, int *span, int *ncombos)
{
  int n = 0;
  *ncombos = 1;
  for (int i = 0; i < d->nops; i++) {
    int64_t l, h;
    if (!xe_imm_domain(d, i, &l, &h)) continue;
    idx[n] = i;
    lo[n] = l;
    span[n] = (int)(h - l + 1);
    *ncombos *= span[n];
    n++;
  }
  return n;
}

typedef int (*xe_extra_fn)(const xts_insn_t *);
typedef void (*xe_vec_hook)(const xts_insn_t *d, xts_rng_t *r, xts_exec_t *x, const int64_t *ops, int t, const char **note);

/* `extra` (may be NULL): number of additional targeted cases for an instruction; they are numbered after the base cases and
 * run with t = 0 .. extra-1 in the hook (t = -1 for the base cases). */
XE_S int xe_vec_enumerate(xts_caselist_t *cl, int (*select)(const xts_insn_t *), int exhaustive_mult, xe_extra_fn extra)
{
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (!select(d)) continue;
    int idx[XTS_MAX_OPS], span[XTS_MAX_OPS], nc;
    int64_t lo[XTS_MAX_OPS];
    unsigned var[8];
    char sfx[24];
    int nimm = xe_imm_combos(d, idx, lo, span, &nc);
    int nv = xe_flag_variants(d, XTS_RC | XTS_T | XTS_A, var);
    int n = nc * nv;
    if (nimm > 1 || nc > 32) n *= exhaustive_mult;      /* multi-field / wide immediates: more than one data draw per combination */
    if (n < XE_DRAWS) n = XE_DRAWS;
    int total = n + (extra ? extra(d) : 0);
    for (int k = 0; k < total; k++) {
      unsigned fl = var[(k / nc) % nv];
      xe_flag_suffix(fl, sfx, sizeof sfx);
      xts_case_t *c = xts_case_addf(cl, "%s%s%s#%d", d->mnem, k >= n ? ".t" : "", sfx, k);
      if (!c) return -1;
      c->insn = id;
      c->flags = fl;
      c->arg[0] = k;
      c->arg[1] = n;
    }
  }
  return 0;
}

XE_S int xe_vec_run_h(const xts_case_t *c, xts_ctx_t *ctx, xe_vec_hook hook)
{
  static xts_exec_t x;
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int idx[XTS_MAX_OPS], span[XTS_MAX_OPS], nc;
  int64_t lo[XTS_MAX_OPS], ops[XTS_MAX_OPS], ov[XTS_MAX_OPS];
  xe_gen_t g;
  const char *note = NULL;
  int nimm = xe_imm_combos(d, idx, lo, span, &nc);
  int t = c->arg[0] >= c->arg[1] ? (int)(c->arg[0] - c->arg[1]) : -1;
  for (int i = 0; i < XTS_MAX_OPS; i++) ov[i] = XE_NOV;
  int rem = (int)(c->arg[0] % nc);
  for (int i = 0; i < nimm; i++) {
    ov[idx[i]] = lo[i] + rem % span[i];
    rem /= span[i];
  }
  xts_case_init(&x);
  xe_gen_defaults(d, &g);
  g.alias_pct = 25;
  if (t >= 0) g.alias_pct = 0;
  if (xe_gen_ops(d, r, &x, ops, ov, &g) < 0) return -1;
  xe_noise(r, &x, d);
  if (hook) hook(d, r, &x, ops, t, &note);
  uint32_t w = xe_encode(d, c->flags, ops);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  xts_semantic_check(ctx, &x, w, note);
  return 0;
}
XE_S int xe_vec_run(const xts_case_t *c, xts_ctx_t *ctx) { return xe_vec_run_h(c, ctx, NULL); }
#endif
