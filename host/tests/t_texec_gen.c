/* texec: case enumeration of the exec.* families and the shared operand generator (src/tests/exec_common.h) */
#include <string.h>
#include <xts/xts.h>
#include "htest.h"
#include <tests/exec_common.h>

static int enumerate(const char *fam, xts_caselist_t *cl)
{
  const xts_family_t *f = xts_family_find(fam);
  if (!f) return -1;
  memset(cl, 0, sizeof *cl);
  return f->enumerate(cl);
}

static int unique_ids(const xts_caselist_t *cl)
{
  for (int i = 0; i < cl->n; i++)
    for (int j = i + 1; j < cl->n && j < i + 3000; j++)
      if (!strcmp(cl->v[i].id, cl->v[j].id)) return 0;
  return 1;
}

static int covers(const xts_caselist_t *cl, int insn)
{
  for (int i = 0; i < cl->n; i++)
    if (cl->v[i].insn == insn) return 1;
  return 0;
}

HTEST(texec_enumerate)
{
  xts_caselist_t cl;
  int n_av = 0, n_128 = 0, n_ill = 0, n_ppc = 0, n_fpu = 0;
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->priv != XTS_PRIV_USER) continue;
    if (d->hw == XTS_HW_ILLEGAL) n_ill++;
    if (d->hw == XTS_HW_EXECUTES && (d->isa == XTS_ISA_ALTIVEC || d->isa == XTS_ISA_CELLVMX)) n_av++;
    if (d->hw == XTS_HW_EXECUTES && d->isa == XTS_ISA_VMX128) n_128++;
  }
  HT_EQ_U64(n_ill, 29);
  HT_EQ_U64(n_av, 145);
  HT_EQ_U64(n_128, 75);

  HT_EQ_U64(enumerate("exec_altivec", &cl), 0);
  HT_CHECK(cl.n > 145 * 24);
  HT_CHECK(unique_ids(&cl));
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->hw == XTS_HW_EXECUTES && (d->isa == XTS_ISA_ALTIVEC || d->isa == XTS_ISA_CELLVMX) && !covers(&cl, id)) { HT_CHECK(0); printf("      missing %s\n", d->mnem); }
  }

  HT_EQ_U64(enumerate("exec_vmx128", &cl), 0);
  HT_CHECK(unique_ids(&cl));
  int pk = 0, up = 0, rl = 0, pw = 0;
  for (int i = 0; i < cl.n; i++) {
    const char *m = xts_insn(cl.v[i].insn)->mnem;
    pk += !strcmp(m, "vpkd3d128"); up += !strcmp(m, "vupkd3d128"); rl += !strcmp(m, "vrlimi128"); pw += !strcmp(m, "vpermwi128");
  }
  HT_CHECK(pk >= 8 * 4 * 4);            /* every type x pack x shift */
  HT_CHECK(up >= 32);                   /* UIMM 0..31 */
  HT_CHECK(rl >= 32 * 4);               /* every mask / bit 4 x rotate */
  HT_CHECK(pw >= 256);                  /* every PERM value */
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->hw == XTS_HW_EXECUTES && d->isa == XTS_ISA_VMX128 && !covers(&cl, id)) { HT_CHECK(0); printf("      missing %s\n", d->mnem); }
  }

  HT_EQ_U64(enumerate("exec_illegal", &cl), 0);
  HT_CHECK(unique_ids(&cl));
  for (int id = 0; id < xts_isa_count; id++)
    if (xts_isa[id].hw == XTS_HW_ILLEGAL && !covers(&cl, id)) { HT_CHECK(0); printf("      missing %s\n", xts_isa[id].mnem); }
  HT_CHECK(covers(&cl, XI_stwcx) && covers(&cl, XI_mulhwx) && covers(&cl, XI_ldux) && covers(&cl, XI_lwzu));

  HT_EQ_U64(enumerate("exec_ppc", &cl), 0);
  HT_CHECK(unique_ids(&cl));
  n_ppc = cl.n;
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if ((d->isa == XTS_ISA_PPC || d->isa == XTS_ISA_XENON) && d->priv == XTS_PRIV_USER && d->hw != XTS_HW_ILLEGAL &&
        d->unit != XTS_UNIT_FPU && !covers(&cl, id) && strcmp(d->mnem, "mtfsb0x") && strcmp(d->mnem, "mtfsb1x") &&
        strcmp(d->mnem, "mcrfs") && strcmp(d->mnem, "mffsx") && strcmp(d->mnem, "mtfsfix") && strcmp(d->mnem, "mtfsfx")) {
      HT_CHECK(0);
      printf("      missing %s\n", d->mnem);
    }
    if (d->hw == XTS_HW_ILLEGAL) HT_CHECK(!covers(&cl, id));
  }
  HT_CHECK(covers(&cl, XI_dcbz128) && covers(&cl, XI_xdcbt) && covers(&cl, XI_sc));

  HT_EQ_U64(enumerate("exec_fpu", &cl), 0);
  HT_CHECK(unique_ids(&cl));
  n_fpu = cl.n;
  HT_CHECK(covers(&cl, XI_fmaddx) && covers(&cl, XI_mtfsfx) && covers(&cl, XI_mffsx) && !covers(&cl, XI_fre));
  HT_CHECK(n_ppc > 4000 && n_fpu > 1000);

  HT_EQ_U64(enumerate("vmx128_regmap", &cl), 0);
  HT_EQ_U64(cl.n, 3 * 128 + 4 + 24 + 256);
  HT_CHECK(unique_ids(&cl));
}

/* every generated word decodes as the instruction it was built for, registers respect the rules */
HTEST(texec_generator)
{
  static xts_exec_t x;
  xts_rng_t rng;
  xts_rng_seed(&rng, 12345);
  int bad = 0, nenc = 0;
  for (int id = 0; id < xts_isa_count; id++) {
    const xts_insn_t *d = &xts_isa[id];
    if (d->priv != XTS_PRIV_USER || (d->hw != XTS_HW_EXECUTES && d->hw != XTS_HW_TRAPS && d->hw != XTS_HW_ILLEGAL)) continue;
    unsigned var[8];
    int nv = xe_flag_variants(d, XTS_RC | XTS_OE | XTS_T | XTS_A, var);
    for (int k = 0; k < 60; k++) {
      int64_t ops[XTS_MAX_OPS];
      xe_gen_t g;
      xts_case_init(&x);
      xe_gen_defaults(d, &g);
      if (xe_gen_ops(d, &rng, &x, ops, NULL, &g) < 0) { bad++; continue; }
      uint32_t w = xe_encode(d, var[k % nv], ops);
      nenc++;
      if (!w || !xts_decodes_as(w, id)) { bad++; printf("    bad word for %s\n", d->mnem); break; }
      for (int i = 0; i < d->nops; i++) {
        const xts_operand_t *o = &d->ops[i];
        if (o->cls == XTS_OC_GPR && (XTS_RESERVED_GPRS & (1u << ops[i]))) { bad++; printf("    reserved gpr in %s\n", d->mnem); }
        if (d->unit == XTS_UNIT_MEMORY && (o->field == XTS_F_RA || o->field == XTS_F_RA0) && o->cls != XTS_OC_FPR && ops[i] == 0 &&
            !(o->field == XTS_F_RA0 && xts_op_index(d, "RB") >= 0)) { bad++; printf("    r0 base in %s\n", d->mnem); }
        if (o->cls == XTS_OC_VRC128 && ops[i] > 7) { bad++; printf("    VC128 > 7 in %s\n", d->mnem); }
      }
    }
  }
  HT_EQ_U64(bad, 0);
  HT_CHECK(nenc > 20000);
}

HTEST(texec_helpers)
{
  /* load-constant sequences of the string-op kernels: 1..5 instructions, never touching another register */
  xts_kbuf_t *k = xk_new(64);
  static const uint64_t v[] = { 0, 5, 0xFFFFFFFFFFFFFFFFull, 0x7FFF, 0x8000, 0x12345678, 0x80000000ull, 0xDEADBEEFCAFEF00Dull, 0xC0DE00000000015Aull };
  for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
    xk_reset(k);
    xk_li64(k, 30, v[i]);
    xk_exit(k);
    HT_EQ_U64(xk_finish(k), 0);
    HT_CHECK(k->n >= 2 && k->n <= 6);
    int ni, nr;
    char why[64];
    HT_EQ_U64(xk_validate(k, &ni, &nr, why, sizeof why), 0);
  }
  xk_free(k);
  /* flag variants */
  unsigned var[8];
  HT_EQ_U64(xe_flag_variants(&xts_isa[XI_addx], XTS_RC | XTS_OE, var), 4);
  HT_EQ_U64(xe_flag_variants(&xts_isa[XI_andx], XTS_RC | XTS_OE, var), 2);
  HT_EQ_U64(xe_flag_variants(&xts_isa[XI_lwz], XTS_RC | XTS_OE, var), 1);
}
