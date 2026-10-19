/* exec.ppc - every user-mode PowerPC instruction the hardware executes (integer, memory, branch, CR, trap), random operands,
 * CPU vs reference model. The scalar FP instructions are in exec.fpu (exec_fpu.c); illegal / invalid forms in exec.illegal.
 *
 * Cases: one per (instruction, flag variant, draw), XE_DRAWS draws per instruction (flag variants cycle through the draws,
 * so Rc / OE combinations are balanced). Operands come from ctx->rng, which the scheduler seeds from (profile seed, family,
 * case id, condition values, repeat): the same case always gets the same inputs.
 *
 * Special groups (all enumerated, none silently dropped):
 *   trap     tw/td/twi/tdi: every TO value (0..31) x 4 operand relations (equal, less, greater, signed/unsigned split, with
 *            random upper halves so tw's 32-bit compare is exercised). Expected: trap (cause 0x02) iff the TO bit matches.
 *   spr      mfspr/mtspr of XER (1), LR (8), CTR (9). Other SPRs are privileged and not touched.
 *   string   lmw/stmw/lswi/lswx/stswi/stswx. The slot cannot restore r1/r2/r13 and r30 holds the state pointer, so
 *              - ranges never contain r1, r2, r13 (lmw/stmw start at r14..r31), r3 is reserved as a temp, RA/RB are r4..r12;
 *              - a range that contains r30 runs as a kernel: mtctr r30 ; [stores: r30 := model input value] ; insn ;
 *                mfctr r30 ; mtctr r3. The case sets CTR = r3 so the final mtctr restores CTR exactly. The model sees a plain
 *                instruction. The bytes loaded into / stored from r30 are thus real (r30's final value is not compared).
 *            lmw/stmw below r14 and ranges that wrap into r1 are NOT run (they would destroy the C stack pointer).
 *   branch   bx/bcx/bclrx/bcctrx: single words in the slot aiming at the landing pads (taken target = pad 1 or pad 2 leaves
 *            a mark in the OUT image; fall-through leaves none). Checked by an in-family oracle (the model has no program
 *            counter): which pad was reached, LR (= slot+4 when LK), CTR decrement, everything else unchanged. All 32 BO
 *            values x 4 random BI / CR / CTR states; BO values that are reserved forms (z bits, a/t = 01, bcctr with BO2=0)
 *            are recorded as info with the prediction, not as pass/fail. Absolute (AA=1) forms are only run when the oracle
 *            says "not taken" (a taken absolute branch would leave the code image); bx with AA=1 is a recorded skip.
 *   stcx     stwcx./stdcx.: (a) no-reservation variant: a clearing stcx is run on the CPU and the model reservation is
 *            cleared first; (b) success variant: a larx of the same line is checked first (its own record), then the stcx.
 *   mftb     value is non-deterministic: info record (monotonic, non-zero), never compared with the model.
 *   skips    sc, eciwx, ecowx and every privileged instruction: one skip record each with the reason. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include <model/model_priv.h>   /* xts_model_resv_clear */
#include "exec_common.h"


enum { KG_GEN = 0, KG_TRAP, KG_SPR, KG_STR, KG_BR, KG_MFTB, KG_SKIP, KG_STCX };
enum { SK_PRIV = 0, SK_SC, SK_EXT, SK_UNTESTED, SK_ABS };
static const char *const skip_why[] = {
  "privileged instruction: the suite never executes it",
  "sc: raises the system-call interrupt, not handled by the case stubs",
  "external control (eciwx/ecowx): needs an external control device",
  "no defined run for this instruction (hardware status untested)",
  "absolute branch: a taken form leaves the code image, a not-taken form is covered by bcx.aa cases",
};
#define TRAP_DRAWS 4
#define SPR_DRAWS 8
#define BR_DRAWS 4
#define BR_AA_DRAWS 2

static int is_mnem(const xts_insn_t *d, const char *m) { return !strcmp(d->mnem, m); }

static int is_fpu(const xts_insn_t *d)
{
  if (d->unit == XTS_UNIT_FPU) return 1;
  return is_mnem(d, "mtfsb0x") || is_mnem(d, "mtfsb1x") || is_mnem(d, "mcrfs") || is_mnem(d, "mffsx") ||
         is_mnem(d, "mtfsfix") || is_mnem(d, "mtfsfx");
}

/* BO classes: 0 architected (z = 0, hint a/t != 01), 1 reserved z bit set, 2 reserved hint a,t = 01 */
static int bo_class(int bo)
{
  static const uint8_t valid[32] = { [0] = 1, [2] = 1, [4] = 1, [6] = 1, [7] = 1, [8] = 1, [10] = 1, [12] = 1, [14] = 1,
                                     [15] = 1, [16] = 1, [18] = 1, [20] = 1, [24] = 1, [25] = 1, [26] = 1, [27] = 1 };
  if (valid[bo]) return 0;
  if (bo == 5 || bo == 13 || bo == 17 || bo == 19) return 2;
  return 1;
}

static xts_case_t *add(xts_caselist_t *cl, const xts_insn_t *d, int kind, unsigned flags, int64_t a0, int64_t a1,
                       int64_t a2, const char *fmt, ...)
{
  char id[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(id, sizeof id, fmt, ap);
  va_end(ap);
  xts_case_t *c = xts_case_add(cl, id);
  if (!c) return NULL;
  c->insn = d->index;
  c->flags = flags;
  c->arg[0] = a0;
  c->arg[1] = a1;
  c->arg[2] = a2;
  c->arg[3] = kind;
  return c;
}

static int exec_enumerate(xts_caselist_t *cl)
{
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    char sfx[24];
    if (d->isa != XTS_ISA_PPC && d->isa != XTS_ISA_XENON) continue;
    if (d->hw == XTS_HW_ILLEGAL) continue;                       /* exec.illegal */
    if (d->priv != XTS_PRIV_USER) {
      if (!add(cl, d, KG_SKIP, 0, 0, SK_PRIV, 0, "skip.%s", d->mnem)) return -1;
      continue;
    }
    if (is_fpu(d)) continue;                                     /* exec.fpu */
    if (is_mnem(d, "sc")) { if (!add(cl, d, KG_SKIP, 0, 0, SK_SC, 0, "skip.sc")) return -1; continue; }
    if (is_mnem(d, "eciwx") || is_mnem(d, "ecowx")) { if (!add(cl, d, KG_SKIP, 0, 0, SK_EXT, 0, "skip.%s", d->mnem)) return -1; continue; }
    if (is_mnem(d, "mftb")) {
      if (!add(cl, d, KG_MFTB, 0, 0, 268, 0, "mftb.tb") || !add(cl, d, KG_MFTB, 0, 0, 269, 0, "mftb.tbu")) return -1;
      continue;
    }
    if (is_mnem(d, "tw") || is_mnem(d, "td") || is_mnem(d, "twi") || is_mnem(d, "tdi")) {
      for (int to = 0; to < 32; to++)
        for (int k = 0; k < TRAP_DRAWS; k++)
          if (!add(cl, d, KG_TRAP, 0, k, to, 0, "%s.to%02d#%d", d->mnem, to, k)) return -1;
      continue;
    }
    if (is_mnem(d, "mfspr") || is_mnem(d, "mtspr")) {
      static const int sprs[] = { 1, 8, 9 };
      static const char *const nm[] = { "xer", "lr", "ctr" };
      for (int s = 0; s < 3; s++)
        for (int k = 0; k < SPR_DRAWS; k++)
          if (!add(cl, d, KG_SPR, 0, k, sprs[s], 0, "%s.%s#%d", d->mnem, nm[s], k)) return -1;
      continue;
    }
    if (is_mnem(d, "lmw") || is_mnem(d, "stmw") || is_mnem(d, "lswi") || is_mnem(d, "lswx") || is_mnem(d, "stswi") ||
        is_mnem(d, "stswx")) {
      for (int k = 0; k < XE_DRAWS; k++)
        if (!add(cl, d, KG_STR, 0, k, 0, 0, "%s#%d", d->mnem, k)) return -1;
      if (is_mnem(d, "lswx") || is_mnem(d, "stswx"))       /* XER byte count 0: lswx leaves RT undefined (KNOWN.md) */
        for (int k = 0; k < 8; k++)
          if (!add(cl, d, KG_STR, 0, k, 1, 0, "%s.count0#%d", d->mnem, k)) return -1;
      continue;
    }
    if (is_mnem(d, "stwcx") || is_mnem(d, "stdcx")) {
      for (int v = 0; v < 2; v++)
        for (int k = 0; k < XE_DRAWS / 2; k++)
          if (!add(cl, d, KG_STCX, 0, k, v, 0, "%s.%s#%d", d->mnem, v ? "resv" : "nores", k)) return -1;
      continue;
    }
    if (is_mnem(d, "bx")) {
      static const int pads[] = { 16, 24, 4 };
      for (unsigned lk = 0; lk <= 1; lk++)
        for (int p = 0; p < 3; p++)
          for (int k = 0; k < 2; k++)
            if (!add(cl, d, KG_BR, lk ? XTS_LK : 0, k, 0, pads[p], "bx%s.p%d#%d", lk ? ".lk" : "", pads[p], k)) return -1;
      if (!add(cl, d, KG_SKIP, 0, 0, SK_ABS, 0, "skip.bx.aa")) return -1;
      continue;
    }
    if (is_mnem(d, "bcx") || is_mnem(d, "bclrx") || is_mnem(d, "bcctrx")) {
      int aa_ok = is_mnem(d, "bcx");
      for (unsigned fl = 0; fl < 4; fl++) {
        unsigned flags = ((fl & 1) ? XTS_LK : 0) | ((fl & 2) ? XTS_AA : 0);
        if ((flags & XTS_AA) && !aa_ok) continue;
        for (int bo = 0; bo < 32; bo++) {
          if ((flags & XTS_AA) && (bo_class(bo) != 0 || (bo & 0x14) == 0x14)) continue;   /* absolute: only plain, can-fall-through forms */
          int n = (flags & XTS_AA) ? BR_AA_DRAWS : BR_DRAWS;
          for (int k = 0; k < n; k++) {
            xe_flag_suffix(flags, sfx, sizeof sfx);
            if (!add(cl, d, KG_BR, flags, k, bo, 16 + 8 * ((k + bo) & 1), "%s%s.bo%02d#%d", d->mnem, sfx, bo, k))
              return -1;
          }
        }
      }
      continue;
    }
    if (d->hw != XTS_HW_EXECUTES && d->hw != XTS_HW_TRAPS) {
      if (!add(cl, d, KG_SKIP, 0, 0, SK_UNTESTED, 0, "skip.%s", d->mnem)) return -1;
      continue;
    }
    unsigned var[8];
    int nv = xe_flag_variants(d, XTS_RC | XTS_OE | XTS_T | XTS_A, var);
    for (int k = 0; k < XE_DRAWS; k++) {
      xe_flag_suffix(var[k % nv], sfx, sizeof sfx);
      if (!add(cl, d, KG_GEN, var[k % nv], k, 0, 0, "%s%s#%d", d->mnem, sfx, k)) return -1;
    }
  }
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- generic */
static int op_index(const xts_insn_t *d, int field)
{
  for (int i = 0; i < d->nops; i++)
    if (d->ops[i].field == field) return i;
  return -1;
}

/* Architecture-driven value fixes applied after the generic operand generation */
static void special_values(const xts_insn_t *d, xts_rng_t *r, xts_exec_t *x, const int64_t *ops)
{
  int ib = op_index(d, XTS_F_RB), ia = op_index(d, XTS_F_RA);
  const char *m = d->mnem;
  if (ib >= 0 && (!strcmp(m, "slwx") || !strcmp(m, "srwx") || !strcmp(m, "srawx") || !strcmp(m, "sldx") || !strcmp(m, "srdx") ||
                  !strcmp(m, "sradx") || !strcmp(m, "rlwnmx") || !strcmp(m, "rldclx") || !strcmp(m, "rldcrx"))) {
    static const uint64_t sh[] = { 0, 1, 7, 8, 31, 32, 33, 63, 64, 65, 127, 128, 0xFFFFFFFF, 0x20, 0x40 };
    if (xe_pct(r, 70)) x->in.gpr[ops[ib]] = xe_pick64(r, sh, XE_N(sh));
  }
  if (ib >= 0 && ia >= 0 && d->ops[ia].field == XTS_F_RA && ops[ia] != ops[ib] &&
      (!strcmp(m, "divwx") || !strcmp(m, "divwux") || !strcmp(m, "divdx") || !strcmp(m, "divdux") || !strcmp(m, "mullwx") ||
       !strcmp(m, "mulldx") || !strcmp(m, "mulhwx") || !strcmp(m, "mulhwux") || !strcmp(m, "mulhdx") || !strcmp(m, "mulhdux")) &&
      xe_pct(r, 35)) {
    static const uint64_t pa[][2] = {
      { 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull }, { 0x80000000ull, 0xFFFFFFFFull }, { 1, 0 }, { 0, 0 },
      { 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull }, { 0x8000000000000000ull, 1 }, { 0x7FFFFFFFull, 2 },
      { 0xFFFFFFFF80000000ull, 0xFFFFFFFFFFFFFFFFull }, { 0x100000000ull, 0x100000000ull }, { 0x7FFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull },
    };
    int i = (int)xts_rng_below(r, XE_N(pa));
    x->in.gpr[ops[ia]] = pa[i][0];
    x->in.gpr[ops[ib]] = pa[i][1];
  }
}

static int run_gen(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int64_t ops[XTS_MAX_OPS];
  xe_gen_t g;
  xts_case_init(&x);
  xe_gen_defaults(d, &g);
  if (xe_gen_ops(d, r, &x, ops, NULL, &g) < 0) return -1;
  special_values(d, r, &x, ops);
  xe_noise(r, &x, d);
  uint32_t w = xe_encode(d, c->flags, ops);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  xts_semantic_check(ctx, &x, w, NULL);
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- trap */
static int run_trap(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int imm = d->nops == 3 && d->ops[2].cls == XTS_OC_SIMM;
  int wide = is_mnem(d, "td") || is_mnem(d, "tdi");
  int64_t ops[XTS_MAX_OPS], ov[XTS_MAX_OPS];
  xe_gen_t g;
  xts_case_init(&x);
  xe_gen_defaults(d, &g);
  g.alias_pct = 0;
  for (int i = 0; i < XTS_MAX_OPS; i++) ov[i] = XE_NOV;
  ov[0] = c->arg[1];
  static const int64_t base[] = { 0, 1, 2, 5, -1, -2, 0x7FFFFFFF, -0x80000000ll, 0x12345678, 100 };
  static const int64_t simm[] = { 0, 1, -1, 5, -32768, 32767, 2, -2 };
  int rel = (int)xts_rng_below(r, 6);
  int64_t a = base[xts_rng_below(r, XE_N(base))], b;
  if (imm) { b = simm[xts_rng_below(r, XE_N(simm))]; ov[2] = b; }
  else b = base[xts_rng_below(r, XE_N(base))];
  switch (rel) {
  case 0: a = b; break;                                  /* equal */
  case 1: a = b - 1 - (int64_t)xts_rng_below(r, 3); break;       /* less */
  case 2: a = b + 1 + (int64_t)xts_rng_below(r, 3); break;       /* greater */
  case 3: a = -1; if (!imm) b = 1; else a = (b > 0) ? -1 : 1; break;   /* signed < but unsigned > (or the reverse) */
  case 4: a = 1; if (!imm) b = -1; else a = (b < 0) ? 1 : -1; break;
  default: a = (int64_t)xts_rng_next(r); if (!imm) b = (int64_t)xts_rng_next(r); break;
  }
  if (xe_gen_ops(d, r, &x, ops, ov, &g) < 0) return -1;
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  if (!wide && !imm) {                                    /* tw: only the low 32 bits are compared, vary the upper half */
    if (xe_pct(r, 40)) ua = (ua & 0xFFFFFFFFull) | (xts_rng_next(r) << 32);
    if (xe_pct(r, 40)) ub = (ub & 0xFFFFFFFFull) | (xts_rng_next(r) << 32);
  } else if (!wide) {
    if (xe_pct(r, 40)) ua = (ua & 0xFFFFFFFFull) | (xts_rng_next(r) << 32);
  }
  xts_set_gpr(&x, (int)ops[1], ua);
  if (!imm) xts_set_gpr(&x, (int)ops[2], ub);
  if (!imm && ops[1] == ops[2]) xts_set_gpr(&x, (int)ops[1], ua);
  xe_noise(r, &x, d);
  uint32_t w = xe_encode(d, 0, ops);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  xts_semantic_check(ctx, &x, w, NULL);
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- spr */
static int run_spr(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int64_t ops[XTS_MAX_OPS], ov[XTS_MAX_OPS];
  xe_gen_t g;
  xts_case_init(&x);
  xe_gen_defaults(d, &g);
  for (int i = 0; i < XTS_MAX_OPS; i++) ov[i] = XE_NOV;
  ov[op_index(d, XTS_F_SPR)] = c->arg[1];
  if (xe_gen_ops(d, r, &x, ops, ov, &g) < 0) return -1;
  xe_noise(r, &x, d);
  if (is_mnem(d, "mtspr")) {
    int rs = (int)ops[op_index(d, XTS_F_RS)];
    uint64_t v = xe_gpr_value(r);
    if (c->arg[1] == 1 && xe_pct(r, 60)) { v = xe_xer[xts_rng_below(r, XE_N(xe_xer))]; v |= xts_rng_next(r) & 0x7F; }
    xts_set_gpr(&x, rs, v);
  } else {
    if (c->arg[1] == 1) { uint64_t v = xe_xer[xts_rng_below(r, XE_N(xe_xer))]; xts_set_xer(&x, v | (xts_rng_next(r) & 0x7F)); }
    if (c->arg[1] == 8) xts_set_lr(&x, xts_rng_next(r));
    if (c->arg[1] == 9) xts_set_ctr(&x, xts_rng_next(r));
  }
  uint32_t w = xe_encode(d, 0, ops);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  xts_semantic_check(ctx, &x, w, NULL);
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- lmw / string ops */
static xts_kbuf_t *str_k;

static int run_str(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  enum { LMW, STMW, LSWI, LSWX, STSWI, STSWX } which;
  const int T = 3;
  uint32_t banned = (1u << 1) | (1u << 2) | (1u << 13) | (1u << T);
  int first = 0, nregs = 0, ra = 4 + (int)xts_rng_below(r, 9), rb = -1;
  int64_t ops[4], dd = 0;
  int bytes = 0, store;
  if (is_mnem(d, "lmw")) which = LMW;
  else if (is_mnem(d, "stmw")) which = STMW;
  else if (is_mnem(d, "lswi")) which = LSWI;
  else if (is_mnem(d, "lswx")) which = LSWX;
  else if (is_mnem(d, "stswi")) which = STSWI;
  else which = STSWX;
  store = which == STMW || which == STSWI || which == STSWX;
  xts_case_init(&x);
  uint64_t tv = xts_rng_next(r);
  xts_set_gpr(&x, T, tv);
  xts_set_ctr(&x, tv);                                   /* the kernel restores CTR with mtctr r3 */
  if (which == LMW || which == STMW) {
    first = 14 + (int)xts_rng_below(r, 18);
    nregs = 32 - first;
    dd = xe_pct(r, 85) ? 4 * ((int64_t)xts_rng_below(r, 33) - 16) : xts_rng_range(r, -64, 64);
    ops[0] = first; ops[1] = dd; ops[2] = ra;
    xts_set_ptr(&x, ra, XE_WINDOW);
  } else {
    static const int nb[] = { 0, 1, 2, 3, 4, 5, 8, 12, 16, 31, 32 };
    if (which == LSWX || which == STSWX) {
      rb = 4 + (int)xts_rng_below(r, 9);
      while (rb == ra) rb = 4 + (int)xts_rng_below(r, 9);
      bytes = xe_pct(r, 60) ? nb[xts_rng_below(r, XE_N(nb))] : (int)xts_rng_below(r, 33);
      if (c->arg[1] == 1) bytes = 0;                       /* .count0 cases: XER[25:31] = 0 */
      banned |= 1u << rb;
    } else {
      int n5 = xe_pct(r, 60) ? nb[xts_rng_below(r, XE_N(nb))] & 31 : (int)xts_rng_below(r, 32);
      bytes = n5 ? n5 : 32;
      ops[2] = n5;
    }
    banned |= 1u << ra;
    nregs = (bytes + 3) / 4;
    for (int t = 0; t < 500; t++) {
      int s = (int)xts_rng_below(r, 32), ok = 1;
      for (int i = 0; i < nregs; i++)
        if (banned & (1u << ((s + i) & 31))) { ok = 0; break; }
      if (ok) { first = s; break; }
      if (t == 499) first = 14;                          /* nregs <= 8: 14.. is always free */
    }
    ops[0] = first;
    ops[1] = ra;
    if (which == LSWX || which == STSWX) {
      ops[2] = rb;
      xts_set_gpr(&x, rb, (uint64_t)xe_memoff[xts_rng_below(r, 9)]);
      xts_set_ptr(&x, ra, XE_WINDOW);
    } else {
      static const int off[] = { 0, 1, 2, 3, 5, 7, 16 };
      xts_set_ptr(&x, ra, XE_WINDOW + off[xts_rng_below(r, XE_N(off))]);
    }
  }
  int uses30 = 0;
  for (int i = 0; i < nregs; i++) {
    int rg = (first + i) & 31;
    if (rg == 30) uses30 = 1;
    if (rg == 1 || rg == 2 || rg == 13) return -1;       /* generator bug: never run */
    xts_set_gpr(&x, rg, xe_gpr_value(r));
  }
  xe_noise(r, &x, d);
  xts_set_gpr(&x, T, tv);                                /* again: the noise may have changed CTR (15% of cases) */
  xts_set_ctr(&x, tv);
  if (which == LSWX || which == STSWX) {
    uint64_t xer = xe_pct(r, 50) ? xe_xer[xts_rng_below(r, XE_N(xe_xer))] : 0;
    xts_set_xer(&x, xer | (uint64_t)bytes);
  }
  uint32_t w = xe_encode(d, 0, ops);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  if (!uses30) {
    xts_semantic_check(ctx, &x, w, NULL);
    return 0;
  }
  if (!str_k && !(str_k = xk_new(64))) return -1;
  xts_kbuf_t *k = str_k;
  xk_reset(k);
  xk_emit(k, XI_mtspr, 0, 9, 30);                        /* mtctr r30: park the state pointer */
  if (store) xk_li64(k, 30, x.in.gpr[30]);          /* the model's value of r30 */
  int pos = xk_emit_a(k, d->index, 0, ops, d->nops);   /* word index of the instruction under test */
  xk_emit(k, XI_mfspr, 0, 30, 9);                        /* mfctr r30 */
  xk_emit(k, XI_mtspr, 0, 9, T);                         /* mtctr r3 (== input CTR) */
  xk_exit(k);
  if (xk_finish(k) < 0) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, w, 1, k->errmsg); return 0; }
  xe_check_kernel(ctx, &x, k, w, pos, "kernel saves/restores r30 via CTR");
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- stwcx. / stdcx. */
static int run_stcx(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x, xc;
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int dword = is_mnem(d, "stdcx");
  int resv = (int)c->arg[2];
  int64_t ops[XTS_MAX_OPS];
  xe_gen_t g;
  xts_case_init(&x);
  xe_gen_defaults(d, &g);
  g.mem_ra0_zero_pct = 0;
  if (xe_gen_ops(d, r, &x, ops, NULL, &g) < 0) return -1;
  if (resv) {
    static const int al[] = { 0, 8, 16, 32, 64, 96, 128 };
    xts_set_gpr(&x, (int)ops[2], (uint64_t)al[xts_rng_below(r, XE_N(al))]);
  }
  xe_noise(r, &x, d);
  uint32_t w = xe_encode(d, 0, ops);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  /* start from a defined reservation state on both sides: a stcx always clears the reservation, whether it stores or not */
  xts_case_init(&xc);
  xts_set_ptr(&xc, 5, XE_WINDOW + 256);
  xts_exec_word(&xc, xts_enc(dword ? XI_stdcx : XI_stwcx, 0, 4, 0, 5));
  xts_model_resv_clear();
  if (resv) {
    int rt = 3;
    while (rt == ops[1] || rt == ops[2]) rt++;
    uint32_t lw = xts_enc(dword ? XI_ldarx : XI_lwarx, 0, rt, (int)ops[1], (int)ops[2]);
    xts_semantic_check(ctx, &x, lw, "larx of the same line, then the stcx");
  }
  xts_semantic_check(ctx, &x, w, resv ? "with reservation" : "no reservation");
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- branches */
static const uint64_t ctr_vals[] = {
  0, 1, 2, 3, 0xFFFFFFFFull, 0x100000000ull, 0x100000001ull, 0x100000002ull, 0x7FFFFFFFull, 0x80000000ull,
  0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x200000000ull,
};

typedef struct { int taken, ctr_ok, cond_ok; uint64_t ctr_new; } bpred_t;

static bpred_t predict(const xts_insn_t *d, int bo, int bi, uint64_t cr, uint64_t ctr, int sf)
{
  bpred_t p;
  int is_ctr = is_mnem(d, "bcctrx");
  int crbit = (int)((cr >> (31 - bi)) & 1);
  int dec = !(bo & 4) && !is_ctr;
  p.ctr_new = dec ? ctr - 1 : ctr;
  uint64_t t = sf ? p.ctr_new : (uint32_t)p.ctr_new;
  p.ctr_ok = (bo & 4) || is_ctr || ((t != 0) ^ !!(bo & 2));
  p.cond_ok = (bo & 0x10) || (crbit == !!(bo & 8));
  p.taken = p.ctr_ok && p.cond_ok;
  return p;
}

static int run_br(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  static xts_state_t exp;
  static xts_state_diff_t diffs[32];
  char det[1536];
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int is_b = is_mnem(d, "bx"), is_bc = is_mnem(d, "bcx"), is_lr = is_mnem(d, "bclrx"), is_ctr = is_mnem(d, "bcctrx");
  int bo = (int)c->arg[1], lk = !!(c->flags & XTS_LK), aa = !!(c->flags & XTS_AA);
  int pad = (int)c->arg[2];
  int bi = (int)xts_rng_below(r, 32);
  uint64_t slot = xts_exec_slot_addr();
  xts_case_init(&x);
  int sf = (int)(x.in.mode & XTS_MODE_SF);
  uint64_t cr = (uint32_t)xts_rng_next(r);
  uint64_t ctr = xe_pct(r, 80) ? ctr_vals[xts_rng_below(r, XE_N(ctr_vals))] : xts_rng_next(r);
  uint64_t target = slot + (uint64_t)pad;
  uint64_t junk = sf ? 0 : (xts_rng_next(r) << 32);      /* 32-bit mode ignores the upper half of LR / CTR */
  int cls = is_b ? 0 : bo_class(bo);
  if (is_ctr && !(bo & 4)) cls = 3;                       /* bcctr with BO2 = 0 is an invalid form */
  if (is_ctr) ctr = target | junk | (xts_rng_below(r, 4));   /* low two bits of the target are ignored */
  if (!is_b) {
    if (xe_pct(r, 50)) cr |= 0x80000000ull >> bi; else cr &= ~(0x80000000ull >> bi);
  }
  bpred_t p;
  p.taken = 1; p.ctr_new = ctr; p.ctr_ok = p.cond_ok = 1;
  if (!is_b) {
    p = predict(d, bo, bi, cr, ctr, sf);
    if (aa && p.taken) {                                  /* absolute branches must not be taken */
      if (!(bo & 0x10)) { if (!!(bo & 8)) cr &= ~(0x80000000ull >> bi); else cr |= 0x80000000ull >> bi; }
      else if (!(bo & 4)) ctr = (bo & 2) ? 2 : 1;
      p = predict(d, bo, bi, cr, ctr, sf);
      if (p.taken) { xe_emit_why(ctx, XTS_ST_SKIP, d->mnem, 0, 0, "could not make the absolute branch fall through"); return 0; }
    }
  }
  xts_set_cr(&x, (uint32_t)cr);
  if (!is_b) xts_set_ctr(&x, ctr);
  uint64_t lr_in = xts_rng_next(r);
  if (is_lr) lr_in = target | junk | xts_rng_below(r, 4);
  xts_set_lr(&x, lr_in);
  uint32_t w;
  if (is_b) w = xts_enc(XI_bx, c->flags, pad);
  else if (is_bc) w = xts_enc(XI_bcx, c->flags, bo, bi, aa ? (pad & ~3) : pad);
  else if (is_lr) w = xts_enc(XI_bclrx, c->flags, bo, bi);
  else w = xts_enc(XI_bcctrx, c->flags, bo, bi);
  if (!w) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands"); return 0; }
  int st = xts_exec_word(&x, w);
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  int status;
  if (st != XTS_EXEC_OK) {
    if (st == XTS_EXEC_DRYRUN || st == XTS_EXEC_SIMULATED) { status = XTS_ST_SKIP; xj_kv_str(&j, "why", "host: branches are not simulated (no program counter in the model)"); }
    else { status = XTS_ST_FAIL; xj_kv_str(&j, "exec_error", x.err); }
    xe_emit_json(ctx, status, d->mnem, w, 1, &j);
    return 0;
  }
  /* expected */
  xe_expect_base(&x, &exp);
  int dec = !is_b && !(bo & 4) && !is_ctr;
  exp.ctr = dec ? ctr - 1 : (is_b ? x.in.ctr : ctr);
  exp.lr = lk ? slot + 4 : lr_in;
  int hit1 = p.taken && pad == XTS_SLOT_PAD1, hit2 = p.taken && pad == XTS_SLOT_PAD2;
  int got1 = x.out.mark[0] != 0, got2 = x.out.mark[1] != 0;
  exp.mark[0] = x.out.mark[0];
  exp.mark[1] = x.out.mark[1];
  int n = xts_state_compare(&exp, &x.out, diffs, 32);
  int nm = xts_mem_compare(x.mem_in, x.mem_out, XTS_SCRATCH_BYTES, NULL, 0);
  int bad = n || nm || x.trap.trapped || got1 != hit1 || got2 != hit2;
  if (cls == 0) status = bad ? XTS_ST_FAIL : XTS_ST_PASS;
  else status = XTS_ST_INFO;
  xj_kv_str(&j, "form", cls == 0 ? "architected" : cls == 1 ? "reserved z bit" : cls == 2 ? "reserved hint at=01" : "bcctr with BO2=0");
  if (status != XTS_ST_PASS) {
    xj_kv_int(&j, "bo", bo);
    xj_kv_int(&j, "bi", bi);
    xj_kv_int(&j, "sf", sf);
    xj_kv_hex(&j, "cr", (uint32_t)cr, 8);
    xj_kv_hex(&j, "ctr_in", ctr, 16);
    xj_kv_hex(&j, "ctr_out", x.out.ctr, 16);
    xj_kv_hex(&j, "lr_in", lr_in, 16);
    xj_kv_hex(&j, "lr_out", x.out.lr, 16);
    xj_kv_hex(&j, "slot", slot, 8);
    xj_kv_bool(&j, "pred_taken", p.taken);
    xj_kv_int(&j, "pred_pad", hit1 ? 1 : hit2 ? 2 : 0);
    xj_kv_int(&j, "got_pad", got1 ? 1 : got2 ? 2 : 0);
    xj_kv_bool(&j, "agrees", !bad);
    xe_put_trap(&j, "cpu", x.trap.trapped, x.trap.vector, x.trap.cause, x.trap.srr0);
    xe_put_diffs(&j, "diffs", diffs, n, 8, "expected", "cpu");
  }
  xe_emit_json(ctx, status, d->mnem, w, 1, &j);
  return 0;
}

/* ---------------------------------------------------------------------------------------------------- mftb / skips */
static int run_mftb(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  char det[400];
  xts_json_t j;
  uint32_t w = xts_enc(XI_mftb, 0, 3, (int)c->arg[1]);
  xts_case_init(&x);
  int st = xts_exec_word(&x, w);
  uint64_t v1 = x.out.gpr[3];
  int trapped = x.trap.trapped;
  int st2 = xts_exec_word(&x, w);
  uint64_t v2 = x.out.gpr[3];
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  int status = XTS_ST_INFO;
  if (st != XTS_EXEC_OK || st2 != XTS_EXEC_OK) {
    status = XTS_ST_SKIP;
    xj_kv_str(&j, "why", "host: mftb is non-deterministic and not modelled");
  } else {
    xj_kv_hex(&j, "first", v1, 16);
    xj_kv_hex(&j, "second", v2, 16);
    int ok = !trapped && !x.trap.trapped && (c->arg[1] == 269 || (v2 >= v1 && v1 != 0));
    xj_kv_bool(&j, "monotonic", ok);
    xj_kv_str(&j, "note", "info: value is non-deterministic, only checked for monotonic / non-zero");
    if (!ok) status = XTS_ST_FAIL;
  }
  xe_emit_json(ctx, status, "mftb", w, 1, &j);
  return 0;
}

static int run_skip(const xts_case_t *c, xts_ctx_t *ctx)
{
  const xts_insn_t *d = xts_insn(c->insn);
  xe_emit_why(ctx, XTS_ST_SKIP, d->mnem, 0, 0, skip_why[c->arg[1]]);
  return 0;
}

static int exec_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  switch ((int)c->arg[3]) {
  case KG_GEN: return run_gen(c, ctx);
  case KG_TRAP: return run_trap(c, ctx);
  case KG_SPR: return run_spr(c, ctx);
  case KG_STR: return run_str(c, ctx);
  case KG_BR: return run_br(c, ctx);
  case KG_MFTB: return run_mftb(c, ctx);
  case KG_SKIP: return run_skip(c, ctx);
  case KG_STCX: return run_stcx(c, ctx);
  }
  return -1;
}

XTS_FAMILY(exec_ppc, .kind = "semantic", .axes = "sf", .enumerate = exec_enumerate, .run = exec_run, .block_cases = 48,
           .desc = "user PowerPC instructions (integer, memory, branch, CR, trap) vs the reference model / in-family oracles");
