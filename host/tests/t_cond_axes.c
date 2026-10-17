/* condition axes of src/conds: apply/revert symmetry, values, applies_to, helpers */
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include "../../src/conds/cond_util.h"
#include "../../src/core/core.h"
#include "htest.h"

static const char *const all_axes[] = { "sf", "fpscr_rn", "fpscr_en", "vscr_nj", "cache", "code_align", "data_align",
                                         "thread_prio", "unroll", "chains", NULL };

static void env_snapshot(xts_env_t *e) { *e = xts_env; }

HTEST(cond_registered)
{
  xts_registry_init(NULL);
  for (int i = 0; all_axes[i]; i++) {
    const xts_axis_t *a = xts_axis_find(all_axes[i]);
    HT_CHECK(a != NULL);
    if (!a) continue;
    HT_CHECK(a->desc && a->desc[0]);
    HT_CHECK(xts_axis_nvalues(a) >= 2);
  }
  HT_CHECK(xts_axis_find("fpscr_ni") == NULL);            /* NI cannot be set on Xenon: no axis */
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("fpscr_rn")), 4);
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("fpscr_en")), 7);
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("vscr_nj")), 2);
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("cache")), 5);
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("code_align")), 6);
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("data_align")), 6);
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("thread_prio")), 4);
  HT_EQ_U64(xts_axis_value_index(xts_axis_find("cache"), "l2"), 3);
  HT_EQ_U64(xts_axis_value_index(xts_axis_find("code_align"), "64"), 5);
}

HTEST(cond_apply_revert_restores_env)
{
  xts_registry_init(NULL);
  for (int i = 0; all_axes[i]; i++) {
    const xts_axis_t *a = xts_axis_find(all_axes[i]);
    if (!a) continue;
    for (int v = 0; v < xts_axis_nvalues(a); v++) {
      xts_env_reset();
      xts_env_t before, during;
      env_snapshot(&before);
      HT_EQ_U64(a->apply(v), 0);
      env_snapshot(&during);
      if (v > 0 && strcmp(a->name, "sf")) HT_CHECK(memcmp(&before, &during, sizeof before) != 0);
      a->revert(v);
      HT_CHECK(memcmp(&before, &xts_env, sizeof before) == 0);
      if (memcmp(&before, &xts_env, sizeof before)) printf("    axis %s value %s left env dirty\n", a->name, a->values[v]);
    }
  }
  xts_env_reset();
}

HTEST(cond_fpscr_bits)
{
  xts_registry_init(NULL);
  xts_exec_t x;
  const xts_axis_t *rn = xts_axis_find("fpscr_rn"), *en = xts_axis_find("fpscr_en"), *nj = xts_axis_find("vscr_nj");
  static const uint32_t en_bits[] = { 0, 0x80, 0x40, 0x20, 0x10, 0x08, 0xF8 };
  xts_env_reset();
  for (int r = 0; r < 4; r++)
    for (int e = 0; e < 7; e++)
      for (int n = 0; n < 2; n++) {
        HT_EQ_U64(rn->apply(r), 0);
        HT_EQ_U64(en->apply(e), 0);
        HT_EQ_U64(nj->apply(n), 0);
        xts_case_init(&x);
        HT_EQ_U64(x.in.fpscr, (uint32_t)r | en_bits[e]);
        HT_EQ_U64(x.in.vscr.w[3], n ? XTS_VSCR_NJ : 0);
        /* reverting one axis keeps the others */
        nj->revert(n);
        en->revert(e);
        xts_case_init(&x);
        HT_EQ_U64(x.in.fpscr, (uint32_t)r);
        rn->revert(r);
        xts_case_init(&x);
        HT_EQ_U64(x.in.fpscr, 0);
        HT_EQ_U64(x.in.vscr.w[3], 0);
      }
  xts_env_reset();
}

HTEST(cond_values_applied)
{
  xts_registry_init(NULL);
  xts_env_reset();
  xts_axis_find("code_align")->apply(3);
  HT_EQ_U64(xts_env.code_offset, 16);
  xts_axis_find("code_align")->revert(3);
  xts_axis_find("cache")->apply(4);
  HT_EQ_U64(xts_env.cache_state, XTS_CACHE_MEM);
  HT_CHECK(!strcmp(xts_cond_cache_name(xts_env.cache_state), "mem"));
  HT_EQ_U64(xts_cond_cache_prepare(NULL, 0), 0);        /* host: no-op */
  xts_axis_find("cache")->revert(4);
  xts_axis_find("thread_prio")->apply(3);
  HT_EQ_U64(xts_env.thread_prio, 3);
  xts_axis_find("thread_prio")->revert(3);
  xts_axis_find("chains")->apply(4);
  HT_EQ_U64(xts_cond_chains(2), 8);
  xts_axis_find("chains")->revert(4);
  HT_EQ_U64(xts_cond_chains(2), 2);
  xts_env_reset();
}

HTEST(cond_data_align)
{
  xts_registry_init(NULL);
  const xts_axis_t *da = xts_axis_find("data_align");
  xts_env_reset();
  HT_EQ_U64(xts_cond_data_offset(8), 0);
  da->apply(1);
  HT_EQ_U64(xts_cond_data_offset(8), 1);
  da->revert(1);
  da->apply(2);
  HT_EQ_U64(xts_cond_data_offset(4), 4);
  da->revert(2);
  da->apply(3);
  for (unsigned w = 2; w <= 16; w *= 2) {
    uint32_t o = xts_cond_data_offset(w);
    HT_CHECK(xts_cond_crosses(o, w, 128));
    HT_CHECK(xts_cond_data_offset(0) == 124);
  }
  HT_EQ_U64(xts_cond_data_offset(1), 0);
  da->revert(3);
  da->apply(4);
  for (unsigned w = 2; w <= 16; w *= 2) HT_CHECK(xts_cond_crosses(xts_cond_data_offset(w), w, 4096));
  HT_CHECK(xts_cond_crosses(xts_cond_data_offset(0), 8, 4096));
  da->revert(4);
  xts_env_reset();
}

HTEST(cond_unroll_sets)
{
  xts_registry_init(NULL);
  const xts_axis_t *un = xts_axis_find("unroll");
  xts_tspec_t s = XTS_TSPEC_DEFAULT;
  xts_env_reset();
  HT_EQ_U64(xts_cond_unroll(&s), 0);                    /* default: spec untouched */
  HT_EQ_U64(s.depths[0], 4);
  for (int v = 1; v < xts_axis_nvalues(un); v++) {
    un->apply(v);
    xts_tspec_t t = XTS_TSPEC_DEFAULT;
    HT_EQ_U64(xts_cond_unroll(&t), 1);
    HT_CHECK(t.ndepths >= 5 && t.ndepths <= XTS_TMAX_DEPTHS);
    for (int i = 1; i < t.ndepths; i++) HT_CHECK(t.depths[i] > t.depths[i - 1]);
    un->revert(v);
  }
  xts_env_reset();
}

static int applies(const char *axis, const char *kind)
{
  const xts_axis_t *a = xts_axis_find(axis);
  return !a->applies_to || a->applies_to("x.y", kind);
}

HTEST(cond_applies_to)
{
  xts_registry_init(NULL);
  HT_CHECK(applies("fpscr_rn", "semantic") && applies("fpscr_en", "semantic") && applies("vscr_nj", "semantic"));
  HT_CHECK(!applies("fpscr_rn", "sched") && !applies("fpscr_en", "mem"));
  HT_CHECK(applies("code_align", "timing") && applies("code_align", "sched") && !applies("code_align", "semantic"));
  HT_CHECK(applies("cache", "mem") && applies("cache", "timing") && !applies("cache", "semantic") && !applies("cache", "calib"));
  HT_CHECK(applies("thread_prio", "timing") && !applies("thread_prio", "semantic"));
  HT_CHECK(applies("unroll", "timing") && !applies("unroll", "sched") && applies("chains", "timing"));
  HT_CHECK(applies("data_align", "semantic") && applies("data_align", "mem") && !applies("data_align", "calib"));
}

/* the scheduler expands the axes for families that list them, and applies_to filters them per kind */
#define ALL_AX "fpscr_rn,fpscr_en,vscr_nj,cache,code_align,data_align,thread_prio,unroll,chains"
static int tc_enum(xts_caselist_t *cl) { xts_case_add(cl, "a"); return 0; }
static int tc_run(const xts_case_t *c, xts_ctx_t *ctx) { (void)c; (void)ctx; return 0; }
XTS_FAMILY(tcond_sem, .kind = "semantic", .axes = ALL_AX, .enumerate = tc_enum, .run = tc_run);
XTS_FAMILY(tcond_tim, .kind = "timing", .axes = ALL_AX, .enumerate = tc_enum, .run = tc_run);
XTS_FAMILY(tcond_mem, .kind = "mem", .axes = ALL_AX, .enumerate = tc_enum, .run = tc_run);

static int plan_blocks(const char *fam, const char *axes)
{
  xts_profile_row_t row = { .families = fam, .axes = axes };
  xts_profile_t prof = { "t", "t", &row, 1 };
  xts_plan_t p;
  if (xts_plan_build(&p, &prof) != 0) return -1;
  int n = p.n;
  free(p.b);
  return n;
}

HTEST(cond_plan_expansion)
{
  xts_registry_init(NULL);
  const char *all = "fpscr_rn=*;fpscr_en=*;vscr_nj=*;cache=*;code_align=*;data_align=*;thread_prio=*;unroll=*;chains=*";
  HT_EQ_U64(plan_blocks("tcond.sem", all), 4 * 7 * 2 * 6);               /* rn x en x nj x data_align */
  HT_EQ_U64(plan_blocks("tcond.tim", "fpscr_rn=*;cache=*;code_align=*;unroll=*;chains=*"), 4 * 5 * 6 * 5 * 6);
  HT_EQ_U64(plan_blocks("tcond.tim", "thread_prio=*;data_align=*"), 4 * 6);
  HT_EQ_U64(plan_blocks("tcond.mem", all), 5 * 6 * 4);                    /* cache x data_align x thread_prio */
  HT_EQ_U64(plan_blocks("tcond.sem", "cache=cold,l2;code_align=*"), 1);   /* neither applies to semantic: one block */
  HT_EQ_U64(plan_blocks("tcond.tim", "cache=cold,l2;code_align=0,64"), 4);
}
