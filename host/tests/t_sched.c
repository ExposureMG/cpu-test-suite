/* registry + scheduler (test families/axis live only in the host-test binary) */
#include <stdlib.h>
#include <string.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

static const char *const t_ax_values[] = { "a", "b", "c", NULL };
static int t_ax_applied = -1;
static int t_ax_apply(int v) { t_ax_applied = v; return 0; }
static void t_ax_revert(int v) { (void)v; t_ax_applied = -1; }
XTS_AXIS(t_ax, .values = t_ax_values, .apply = t_ax_apply, .revert = t_ax_revert, .order = 50, .desc = "host-test axis");

static int ts_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 10; i++) xts_case_addf(cl, "s%02d", i);
  return 0;
}
static int ts_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  (void)c;
  xts_emit_value(ctx, "applied", t_ax_applied, "", XTS_ST_PASS);
  return 0;
}
XTS_FAMILY(t_sched, .kind = "test", .axes = "t_ax", .enumerate = ts_enum, .run = ts_run, .block_cases = 4);

HTEST(registry)
{
  HT_EQ_U64(xts_registry_init(NULL), 0);
  HT_CHECK(xts_family_find("t.sched") != NULL);
  HT_CHECK(xts_family_find("exec.ppc") != NULL);
  HT_CHECK(xts_family_find("self.exec") != NULL);
  for (int i = 1; i < xts_family_count(); i++)
    HT_CHECK(strcmp(xts_family_get(i - 1)->name, xts_family_get(i)->name) < 0);
  HT_CHECK(xts_axis_find("sf") != NULL && xts_axis_find("t_ax") != NULL);
  HT_EQ_U64(xts_axis_nvalues(xts_axis_find("t_ax")), 3);
  HT_EQ_U64(xts_axis_value_index(xts_axis_find("sf"), "32"), 1);
  HT_CHECK(xts_family_has_axis(xts_family_find("t.sched"), "t_ax"));
  HT_CHECK(!xts_family_has_axis(xts_family_find("t.sched"), "sf"));
  HT_CHECK(xts_family_has_axis(xts_family_find("exec.ppc"), "sf"));
  int a = xts_env_slot("x_axis"), b = xts_env_slot("y_axis");
  HT_CHECK(a >= 0 && b >= 0 && a != b && xts_env_slot("x_axis") == a);
}

HTEST(scheduler_plan)
{
  static const xts_profile_row_t rows[] = {
    { .families = "t.sched", .axes = "t_ax=a,c;sf=*;no_such_axis=1", .repeats = 2, .seed = 5 },
    { .families = "nomatch.*" },
    { .families = "t.*", .kind = "other" },
  };
  static const xts_profile_t prof = { "t", "test", rows, 3 };
  xts_plan_t p;
  HT_EQ_U64(xts_plan_build(&p, &prof), 0);
  HT_EQ_U64(p.n, 6);                     /* 2 axis values x 3 blocks (10 cases / 4) - sf ignored for t.sched */
  HT_EQ_U64(p.total_cases, 40);          /* 10 cases x 2 values x 2 repeats */
  for (int i = 0; i < p.n; i++)
    for (int k = 0; k < i; k++) HT_CHECK(strcmp(p.b[i].key, p.b[k].key));
  HT_CHECK(!strcmp(p.b[0].key, "r0:t.sched:t_ax=a:c0"));
  HT_CHECK(!strcmp(p.b[5].key, "r0:t.sched:t_ax=c:c2"));
  HT_EQ_U64(p.b[2].ncases, 2);
  xts_condvec_t cv;
  xts_plan_condvec(&p.b[3], &cv);
  HT_CHECK(!strcmp(xts_cond_get(&cv, "t_ax"), "c"));
  HT_CHECK(!strcmp(xts_cond_get(&cv, "sf"), "64"));    /* not applied: default */
  int fi = p.b[0].fam;
  const xts_case_t *c = &p.cases[fi].v[1];
  uint64_t s0 = xts_case_seed(&p.b[0], c, 0), s1 = xts_case_seed(&p.b[0], c, 1), s2 = xts_case_seed(&p.b[3], c, 0);
  HT_CHECK(s0 != s1 && s0 != s2);
  HT_EQ_U64(xts_case_seed(&p.b[0], c, 0), s0);
}

HTEST(scheduler_sweep_rows)
{
  xts_registry_init(NULL);
  /* a sweep row skips families that are not sensitive to any of its axes; a plain row runs them at their defaults */
  static const xts_profile_row_t rows[] = {
    { .families = "t.sched", .axes = "sf=*", .sweep = 1 },
    { .families = "t.sched", .axes = "sf=*" },
    { .families = "t.sched", .axes = "t_ax=b", .sweep = 1 },
  };
  static const xts_profile_t prof = { "t", "test", rows, 3 };
  xts_plan_t p;
  HT_EQ_U64(xts_plan_build(&p, &prof), 0);
  HT_EQ_U64(p.n, 6);                     /* row 0: none; row 1: 3 default blocks; row 2: 3 blocks at t_ax=b */
  HT_CHECK(!strcmp(p.b[0].key, "r1:t.sched:-:c0"));
  HT_CHECK(!strcmp(p.b[3].key, "r2:t.sched:t_ax=b:c0"));
}
