/* exec.illegal - words the Xenon core must reject with an illegal-instruction program interrupt (vector 0x700, SRR1 cause 0x08):
 *   hw       every instruction whose hardware.status is "illegal" (29: the 22 dropped AltiVec ops, lvsl128 / lvsr128, popcntb,
 *            mcrxr, dcba, fre, frsqrtes), 4 operand draws each
 *   ra0      update-form INDEXED loads / stores with RA = 0 (the invalid form "RA = 0"): lbzux lhzux lhaux lwzux lwaux ldux
 *            stbux sthux stwux stdux lfsux lfdux stfsux stfdux. RB holds a scratch pointer (never r0) and r0 is 0, so an executing
 *            CPU stays in memory whether it reads RA = 0 as the literal 0 or as GPR[0]. Every word of the family also goes through
 *            xts_exec_mem_guard (EA window inside scratch/big whatever the model says); an unsafe one is a skip, never executed.
 *   rart     update-form loads with RA = RT (the other invalid form), indexed AND D-form (lbzu lhzu lhau lwzu ldu): kept
 *            separate from ra0 (the old run never separated them)
 *   stcx0    stwcx. / stdcx. with Rc = 0
 *   mulh     mulhw / mulhwu / mulhd / mulhdu with OE = 1 and Rc = 1 (invalid), OE = 1 / Rc = 0 as control, plus the valid
 *            OE = 0 forms as control
 * Every word is delivered through a kernel as a deliberate data word (xk_data), never as code (check_illegal enforces that for
 * the ELF). Pass / fail: the invalid classes must trap with cause 0x08 at the word (kernel offset 0); the controls must NOT
 * trap. The actual SRR1 cause, vector, whether the word executed, and how many registers changed are always in the detail.
 * On the host nothing executes: the record is a skip carrying the model's verdict.
 * Not run: D-form update loads / stores with RA = 0 (the EA would be the bare displacement, unmapped page zero). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "exec_common.h"

enum { C_HW = 0, C_RA0, C_RART, C_STCX0, C_MULH };

static const char *const ra0_list[] = { "lbzux", "lhzux", "lhaux", "lwzux", "lwaux", "ldux", "stbux", "sthux", "stwux", "stdux",
                                        "lfsux", "lfdux", "stfsux", "stfdux", NULL };
static const char *const rart_list[] = { "lbzux", "lhzux", "lhaux", "lwzux", "lwaux", "ldux", "lbzu", "lhzu", "lhau", "lwzu", "ldu", NULL };
static const char *const mulh_list[] = { "mulhwx", "mulhwux", "mulhdx", "mulhdux", NULL };

static int in_list(const char *const *l, const char *m)
{
  for (int i = 0; l[i]; i++)
    if (!strcmp(l[i], m)) return 1;
  return 0;
}

static xts_case_t *addc(xts_caselist_t *cl, const xts_insn_t *d, int cls, int a1, int draws_k, const char *tag)
{
  xts_case_t *c = xts_case_addf(cl, "%s.%s#%d", d->mnem, tag, draws_k);
  if (!c) return NULL;
  c->insn = d->index;
  c->arg[0] = draws_k;
  c->arg[1] = a1;
  c->arg[3] = cls;
  return c;
}

static int ill_enumerate(xts_caselist_t *cl)
{
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->priv != XTS_PRIV_USER) continue;
    if (d->hw == XTS_HW_ILLEGAL)
      for (int k = 0; k < 4; k++)
        if (!addc(cl, d, C_HW, 0, k, "hw")) return -1;
    if (in_list(ra0_list, d->mnem))
      for (int k = 0; k < 4; k++)
        if (!addc(cl, d, C_RA0, 0, k, "ra0")) return -1;
    if (in_list(rart_list, d->mnem))
      for (int k = 0; k < 4; k++)
        if (!addc(cl, d, C_RART, 0, k, "rart")) return -1;
    if (!strcmp(d->mnem, "stwcx") || !strcmp(d->mnem, "stdcx"))
      for (int k = 0; k < 6; k++)
        if (!addc(cl, d, C_STCX0, 0, k, "rc0")) return -1;
    if (in_list(mulh_list, d->mnem)) {
      static const char *const tag[] = { "oe1rc1", "oe1rc0", "oe0rc1", "oe0rc0" };
      for (int v = 0; v < 4; v++)
        for (int k = 0; k < 3; k++)
          if (!addc(cl, d, C_MULH, v, k, tag[v])) return -1;
    }
  }
  return 0;
}

static xts_kbuf_t *kb;

static int ill_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  static xts_exec_t x;
  static xts_state_t mexp;
  static xts_state_diff_t diffs[16];
  char det[1024];
  const xts_insn_t *d = xts_insn(c->insn);
  xts_rng_t *r = &ctx->rng;
  int cls = (int)c->arg[3], expect_illegal = 1, nops = d->nops;
  int64_t ops[XTS_MAX_OPS], ov[XTS_MAX_OPS];
  xe_gen_t g;
  unsigned flags = 0;
  const char *what = "";
  for (int i = 0; i < XTS_MAX_OPS; i++) ov[i] = XE_NOV;
  xts_case_init(&x);
  xe_gen_defaults(d, &g);
  g.alias_pct = 0;
  g.mem_ra0_zero_pct = 0;
  switch (cls) {
  case C_HW: {
    unsigned var[8];
    int nv = xe_flag_variants(d, XTS_RC | XTS_OE | XTS_T | XTS_A, var);
    flags = var[c->arg[0] % nv];
    what = "hardware-illegal word";
    break;
  }
  case C_RA0: {
    int ia = -1;
    for (int i = 0; i < nops; i++)
      if (d->ops[i].field == XTS_F_RA) ia = i;
    ov[ia] = 0;
    what = "update-form indexed, RA = 0";
    break;
  }
  case C_RART:
    what = "update-form load, RA = RT";
    break;
  case C_STCX0: what = "stcx. with Rc = 0"; break;
  case C_MULH:
    what = "mulh with the OE bit";
    flags = (c->arg[1] == 0 || c->arg[1] == 2) ? XTS_RC : 0;     /* 0 oe1rc1, 1 oe1rc0, 2 oe0rc1, 3 oe0rc0 */
    expect_illegal = c->arg[1] == 0;
    break;
  }
  if (cls == C_RART) {
    /* RT and RA are the same register: generate normally, then force RA := RT */
    if (xe_gen_ops(d, r, &x, ops, ov, &g) < 0) return -1;
    int ird = -1, ira = -1, irb = -1;
    for (int i = 0; i < nops; i++) {
      if (d->ops[i].field == XTS_F_RD) ird = i;
      if (d->ops[i].field == XTS_F_RA || d->ops[i].field == XTS_F_RA0) ira = i;
      if (d->ops[i].field == XTS_F_RB) irb = i;
    }
    ops[ird] = ops[ira];                                 /* RT := the pointer register */
    (void)irb;
  } else if (cls == C_RA0) {
    if (xe_gen_ops(d, r, &x, ops, ov, &g) < 0) return -1;
    /* RB becomes the scratch pointer (EA = 0 + RB if the CPU executes the word). If a core read the RA = 0 field as GPR[0] instead
     * of the literal 0, EA = r0 + RB: r0 is zeroed (not the canary) and RB is never r0 itself, so both readings land in scratch. */
    int irb = -1;
    for (int i = 0; i < nops; i++)
      if (d->ops[i].field == XTS_F_RB) irb = i;
    if (ops[irb] == 0) {
      int taken[32] = { 0 };
      for (int i = 0; i < nops; i++)
        if (ops[i] >= 0 && ops[i] < 32 && i != irb) taken[ops[i]] = 1;
      int nr = 3;
      while (nr < 30 && (taken[nr] || (XTS_RESERVED_GPRS & (1u << nr)))) nr++;
      ops[irb] = nr;
    }
    xts_set_gpr(&x, 0, 0);
    xts_set_ptr(&x, (int)ops[irb], XE_WINDOW + xe_memoff[xts_rng_below(r, 8)]);
  } else {
    if (xe_gen_ops(d, r, &x, ops, ov, &g) < 0) return -1;
  }
  xe_noise(r, &x, d);
  uint32_t w = xe_encode(d, flags, ops);
  if (!w) {
    /* the encoder refuses invalid forms it knows about (e.g. stcx. Rc): build from the valid word */
    xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, 0, 0, "encoder rejected the generated operands");
    return 0;
  }
  if (cls == C_STCX0) w &= ~1u;
  if (cls == C_MULH && (c->arg[1] == 0 || c->arg[1] == 1)) w |= 0x400u;       /* OE = 1 (a reserved bit for mulh*) */
  const int control = !expect_illegal;
  if (!kb && !(kb = xk_new(16))) return -1;
  xk_reset(kb);
  xk_data(kb, w);
  xk_exit(kb);
  if (xk_finish(kb) < 0) { xe_emit_why(ctx, XTS_ST_FAIL, d->mnem, w, 1, kb->errmsg); return 0; }
  xts_mem_t m;
  uint8_t mem_exp[XTS_SCRATCH_BYTES];
  xts_mem_init(&m);
  memcpy(mem_exp, x.mem_in, sizeof mem_exp);
  xts_mem_add(&m, xts_scratch_addr(), mem_exp, sizeof mem_exp);
  xts_mres_t mr = xts_model_exec(NULL, w, &x.in, &m, &mexp);
  /* expected-illegal words trap before they touch memory on hardware that rejects them, but if a core revision executes one the
   * access must land in test memory: the memory guard runs for every word here, whatever the model says, and a word whose inputs
   * would reach outside scratch/big is not executed (the generator keeps the inputs inside, so this is a safety net) */
  char guard[200];
  int guarded = xts_exec_mem_guard(xts_decode1(w), w, &x.in, mr, m.fault_ea, guard, sizeof guard);
  int st = guarded ? XTS_EXEC_NOTRUN : xts_exec_kernel(&x, kb);
  xts_json_t j;
  xj_init(&j, det, sizeof det);
  xj_obj(&j);
  xj_kv_str(&j, "class", what);
  xj_kv_str(&j, "expect", expect_illegal ? "illegal (cause 0x08)" : "no trap (control)");
  xj_kv_str(&j, "model", xts_mres_name(mr));
  if (guarded) {
    xj_kv_str(&j, "why", guard);
    xe_emit_json(ctx, XTS_ST_SKIP, d->mnem, w, 1, &j);
    return 0;
  }
  int status;
  if (st == XTS_EXEC_DRYRUN || st == XTS_EXEC_SIMULATED) {
    status = XTS_ST_SKIP;
    xj_kv_str(&j, "why", "host dry run: word built and delivered through xk_data, not executed");
  } else if (st < 0) {
    status = XTS_ST_FAIL;
    xj_kv_str(&j, "exec_error", x.err);
  } else {
    const char *obs;
    int illegal = x.trap.trapped && x.trap.vector == 0x700 && x.trap.cause == XTS_CAUSE_ILLEGAL;
    if (illegal) obs = "illegal";
    else if (x.trap.trapped) obs = "other interrupt";
    else obs = "executed";
    int n = xts_state_compare(&x.in, &x.out, diffs, 16) + xts_mem_compare(x.mem_in, x.mem_out, XTS_SCRATCH_BYTES, NULL, 0);
    int at_word = !x.trap.trapped || (x.trap.where == 1 && x.trap.offset == 0);
    xj_kv_str(&j, "observed", obs);
    xj_kv_bool(&j, "trapped", x.trap.trapped);
    xj_kv_hex(&j, "vector", x.trap.vector, 4);
    xj_kv_hex(&j, "cause", x.trap.cause, 2);
    xj_kv_hex(&j, "srr1", x.trap.srr1, 16);
    xj_kv_int(&j, "changed", n);
    xj_kv_bool(&j, "at_word", at_word);
    if (expect_illegal) status = (illegal && at_word) ? XTS_ST_PASS : XTS_ST_FAIL;
    else status = (!x.trap.trapped) ? XTS_ST_PASS : XTS_ST_FAIL;
    if (control) xj_kv_bool(&j, "control", 1);
  }
  xe_emit_json(ctx, status, d->mnem, w, 1, &j);
  return 0;
}

XTS_FAMILY(exec_illegal, .kind = "semantic", .axes = "sf", .enumerate = ill_enumerate, .run = ill_run, .block_cases = 32,
           .desc = "hardware-illegal words and invalid instruction forms must raise the illegal-instruction interrupt (0x08)");
