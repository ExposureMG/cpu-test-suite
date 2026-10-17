/* tlat families (lat.chain, tput.indep, lat.data): kernels decode as intended, never touch reserved registers, have the
 * dependency structure they claim, and (through the reference model) keep their data in the intended class. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xts/xts.h>
#include "htest.h"
#include "../../src/tests/timing_ops.h"

static const xts_caselist_t *cases_of(const char *fam)
{
  static xts_caselist_t cl[3];
  static const char *names[3];
  for (int i = 0; i < 3; i++)
    if (names[i] && !strcmp(names[i], fam)) return &cl[i];
  for (int i = 0; i < 3; i++) {
    if (names[i]) continue;
    const xts_family_t *f = xts_family_find(fam);
    if (!f || f->enumerate(&cl[i]) < 0) return NULL;
    names[i] = fam;
    return &cl[i];
  }
  return NULL;
}

static const xts_case_t *find_case(const xts_caselist_t *cl, const char *id)
{
  for (int i = 0; cl && i < cl->n; i++)
    if (!strcmp(cl->v[i].id, id)) return &cl->v[i];
  return NULL;
}

/* every word decodes as its instruction; GPR operands avoid r1 r2 r13 r27-r30 and RA0 is never r0 */
static int words_ok(const tlat_kern_t *k, char *why, int n)
{
  for (int i = 0; i < k->npat; i++) {
    const tlat_word_t *w = &k->pat[i];
    uint32_t word = xts_enc_a(w->insn, w->flags, w->ops, w->nops);
    if (!word || !xts_decodes_as(word, w->insn)) { snprintf(why, n, "word %d does not decode", i); return 0; }
    const xts_insn_t *d = &xts_isa[w->insn];
    for (int o = 0; o < d->nops; o++) {
      int cls = d->ops[o].cls;
      if (cls != XTS_OC_GPR && cls != XTS_OC_GPR0) continue;
      int r = (int)xts_extract(w->insn, word, o);
      if (((XTS_RESERVED_GPRS >> r) & 1) || (r >= 27 && r <= 29) || (cls == XTS_OC_GPR0 && r == 0)) {
        snprintf(why, n, "%s uses r%d", d->mnem, r);
        return 0;
      }
    }
  }
  return 1;
}

/* the register written by word i is read by word i+1 through `slot` (rotation, chain), or by itself (acc) */
static int dep_ok(const tlat_shape_t *s, const tlat_kern_t *k, int slot)
{
  for (int i = 0; i < k->npat; i++) {
    const tlat_word_t *w = &k->pat[i], *nx = &k->pat[(i + 1) % k->npat];
    int64_t dst = w->ops[s->dest];
    if (slot == TLAT_SLOT_ACC) {
      if (s->tied_alias >= 0 && w->ops[s->tied_alias] != dst) return 0;
    } else if (nx->ops[s->src[slot]] != dst) {
      return 0;
    }
  }
  return 1;
}

HTEST(tlat_kernels)
{
  const xts_caselist_t *lc = cases_of("lat.chain"), *tp = cases_of("tput.indep"), *ld = cases_of("lat.data");
  HT_CHECK(lc && tp && ld);
  if (!lc || !tp || !ld) return;
  printf("    lat.chain %d cases, tput.indep %d, lat.data %d\n", lc->n, tp->n, ld->n);
  static const char *const must[] = { "addx:RA", "addx:RB", "addx.oe.rc:RB", "fmaddx:FB", "vmaddfp128:VA128",
                                      "vmaddfp128:acc", "rlwimix:RS", "rlwimix:acc", "crand:CRBA", "mcrf:CRFS",
                                      "vperm128:VC128", "vsel128:acc", "vrlimi128:VB128", "divwx:RB", NULL };
  for (int i = 0; must[i]; i++)
    if (!find_case(lc, must[i])) { printf("    missing lat.chain case %s\n", must[i]); HT_CHECK(0); }
  HT_CHECK(find_case(tp, "mullwx.oe") != NULL);
  HT_CHECK(find_case(ld, "divwx:by_one") && find_case(ld, "vaddfp:denormal") && find_case(ld, "fmaddx:denormal_out"));
  HT_CHECK(!find_case(lc, "lwz:RA0") && !find_case(lc, "cmp:RA") && !find_case(lc, "lvsl:RB"));
  static tlat_kern_t k;
  char why[96];
  int bad = 0;
  for (int i = 0; i < lc->n; i++) {
    const xts_case_t *c = &lc->v[i];
    tlat_shape_t s;
    int slot = (int)c->arg[0];
    if (tlat_shape(c->insn, &s, NULL) < 0 || tlat_build_chain(&s, c->flags, slot, &k) < 0) {
      printf("    %s: build failed %s\n", c->id, k.errmsg);
      bad++;
      continue;
    }
    if (!words_ok(&k, why, sizeof why) || !dep_ok(&s, &k, slot)) {
      printf("    %s: %s\n", c->id, why);
      bad++;
    }
    for (int d = 0; d < TLAT_NDEPTHS; d++)
      if ((tlat_depths[d] * k.per_unit) % k.npat) bad++;
  }
  HT_EQ_U64(bad, 0);
  bad = 0;
  int skipped = 0;
  for (int i = 0; i < tp->n; i++) {
    const xts_case_t *c = &tp->v[i];
    tlat_shape_t s;
    tlat_shape(c->insn, &s, NULL);
    static const int ns[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
    for (int q = 0; q < 8; q++) {
      if (tlat_build_nchains(&s, c->flags, ns[q], &k) < 0) { skipped++; continue; }
      int distinct = 1;
      for (int a = 0; a < k.npat; a++)
        for (int b = a + 1; b < k.npat; b++)
          if (k.pat[a].ops[s.dest] == k.pat[b].ops[s.dest]) distinct = 0;
      int self = 1;
      for (int a = 0; a < k.npat; a++)
        if (!s.tied && k.pat[a].ops[s.src[0]] != k.pat[a].ops[s.dest]) self = 0;
      if (!words_ok(&k, why, sizeof why) || !distinct || !self || k.npat != ns[q]) {
        printf("    %s n=%d: %s distinct %d self %d\n", c->id, ns[q], why, distinct, self);
        bad++;
      }
    }
  }
  HT_EQ_U64(bad, 0);
  printf("    tput.indep: %d (case, N) points skipped for small register pools\n", skipped);
  HT_CHECK(skipped < 8);         /* only mcrf N=12,16 */
  HT_CHECK(tlat_build_filler(8, 3, &k) == 0 && words_ok(&k, why, sizeof why) && k.any_len);
  bad = 0;
  for (int i = 0; i < ld->n; i++) {
    for (int chain = 0; chain < 2; chain++) {
      int e = tlat_ld_kernel(&ld->v[i], chain, 0, 0, &k);
      if (e < 0 || (e > 0 && !words_ok(&k, why, sizeof why))) { printf("    lat.data %s chain %d: %s\n", ld->v[i].id, chain, e < 0 ? "build" : why); bad++; }
    }
  }
  HT_EQ_U64(bad, 0);
}

/* ---- data stability through the reference model ---- */
enum { C_ZERO, C_DEN, C_NORM, C_INF, C_NAN };
static int fclass64(uint64_t u)
{
  uint64_t e = (u >> 52) & 0x7FF, m = u & 0xFFFFFFFFFFFFFull;
  return e == 0 ? (m ? C_DEN : C_ZERO) : e == 0x7FF ? (m ? C_NAN : C_INF) : C_NORM;
}
static int fclass32(uint32_t u)
{
  uint32_t e = (u >> 23) & 0xFF, m = u & 0x7FFFFF;
  return e == 0 ? (m ? C_DEN : C_ZERO) : e == 0xFF ? (m ? C_NAN : C_INF) : C_NORM;
}
static int vclass(const xts_vr_t *v, int *mixed)
{
  int c = fclass32(v->w[0]);
  *mixed = 0;
  for (int i = 1; i < 4; i++)
    if (fclass32(v->w[i]) != c) *mixed = 1;
  return c;
}
static int reg_class(const tlat_shape_t *s, const xts_state_t *st, int reg, int *mixed)
{
  *mixed = 0;
  return s->cls == TLAT_FPR ? fclass64(st->fpr[reg]) : vclass(&st->vr[reg], mixed);
}

/* run the pattern `steps` words on the model; returns steps run (0 = model does not implement it) and the class history */
static int simulate(const tlat_kern_t *k, int steps, int reg, const tlat_shape_t *s, int *seen)
{
  static xts_exec_t x;
  xts_env_reset();
  xts_case_init(&x);
  tlat_setup(&x, (void *)k);
  for (int i = 0; i < steps; i++) {
    const tlat_word_t *w = &k->pat[i % k->npat];
    if (xts_exec_word(&x, xts_enc_a(w->insn, w->flags, w->ops, w->nops)) != XTS_EXEC_SIMULATED || x.trap.trapped) return 0;
    x.in = x.out;
    int mixed, c = reg_class(s, &x.out, reg, &mixed);
    seen[c] = 1;
    if (mixed) seen[5] = 1;
  }
  return steps;
}

HTEST(tlat_neutral_data)
{
  const xts_caselist_t *lc = cases_of("lat.chain");
  HT_CHECK(lc != NULL);
  if (!lc) return;
  static tlat_kern_t k;
  int sim = 0, unimpl = 0, bad = 0;
  for (int i = 0; i < lc->n; i++) {
    const xts_case_t *c = &lc->v[i];
    tlat_shape_t s;
    if (tlat_shape(c->insn, &s, NULL) < 0 || (s.cls != TLAT_FPR && s.cls != TLAT_VR)) continue;
    const tlat_fpinfo_t *fi = tlat_fpinfo(c->insn);
    if (!fi || fi->fam == TLAT_FAM_CMP || fi->fam == TLAT_FAM_I2F) continue;   /* masks / integer bits: not FP data */
    int slot = (int)c->arg[0];
    if (tlat_build_chain(&s, c->flags, slot, &k) < 0) { bad++; continue; }
    int seen[6] = { 0 };
    if (!simulate(&k, 96, k.chain_reg[0], &s, seen)) { unimpl++; continue; }
    sim++;
    const char *note = tlat_neutral_note(&s, slot);
    int okk = !seen[C_DEN] && (note || (!seen[C_INF] && !seen[C_NAN])) && (note || fi->fam == TLAT_FAM_F2I || !seen[C_ZERO]);
    if (!okk) {
      printf("    %s: data left the normal class (den %d zero %d inf %d nan %d)\n", c->id, seen[C_DEN], seen[C_ZERO], seen[C_INF],
             seen[C_NAN]);
      bad++;
    }
  }
  printf("    lat.chain FP/vector chains: %d simulated, %d not in the model\n", sim, unimpl);
  HT_EQ_U64(bad, 0);
  HT_CHECK(sim > 60);

  /* lat.data identity chains keep the bin's class */
  const xts_caselist_t *ld = cases_of("lat.data");
  HT_CHECK(ld != NULL);
  if (!ld) return;
  sim = unimpl = bad = 0;
  for (int i = 0; i < ld->n; i++) {
    const xts_case_t *c = &ld->v[i];
    if (tlat_ld_kernel(c, 1, 0, 0, &k) <= 0) continue;
    tlat_shape_t s;
    tlat_shape(c->insn, &s, NULL);
    int reg = k.chain_reg[0], mixed;
    static xts_exec_t x0;
    xts_env_reset();
    xts_case_init(&x0);
    tlat_setup(&x0, &k);
    int want = reg_class(&s, &x0.in, reg, &mixed);
    int seen[6] = { 0 };
    if (!simulate(&k, 16, reg, &s, seen)) { unimpl++; continue; }
    sim++;
    for (int q = 0; q < 5; q++)
      if (seen[q] && q != want) {
        printf("    lat.data %s chain: class %d -> %d\n", c->id, want, q);
        bad++;
        break;
      }
  }
  printf("    lat.data identity chains: %d simulated, %d not in the model\n", sim, unimpl);
  HT_EQ_U64(bad, 0);
  HT_CHECK(sim > 40);

  /* lat.data indep kernels of tied accumulators: the accumulator feeds itself, so it must stay in its bin's class */
  sim = bad = 0;
  for (int i = 0; i < ld->n; i++) {
    const xts_case_t *c = &ld->v[i];
    tlat_shape_t s;
    if (tlat_shape(c->insn, &s, NULL) < 0 || !s.tied || (s.cls != TLAT_FPR && s.cls != TLAT_VR)) continue;
    if (tlat_ld_kernel(c, 0, 0, 0, &k) <= 0) continue;
    int reg = k.chain_reg[0], mixed;
    static xts_exec_t x0;
    xts_env_reset();
    xts_case_init(&x0);
    tlat_setup(&x0, &k);
    int want = reg_class(&s, &x0.in, reg, &mixed);
    int seen[6] = { 0 };
    if (!simulate(&k, 8 * k.npat, reg, &s, seen)) continue;
    sim++;
    for (int q = 0; q < 5; q++)
      if (seen[q] && q != want) {
        printf("    lat.data %s indep accumulator: class %d -> %d\n", c->id, want, q);
        bad++;
        break;
      }
  }
  printf("    lat.data tied indep accumulators: %d simulated\n", sim);
  HT_EQ_U64(bad, 0);
}
