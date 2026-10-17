/* tsys: branch / sysops / calib families - analysis maths with planted data, case lists. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <xts/xts.h>
#include "htest.h"
#include "../../src/tests/tsys_priv.h"

HTEST(tsys_mispredict_formula)
{
  /* never 10, always 12 -> a perfect predictor pays 11; 50 % mispredicts of penalty 25 -> 11 + 12.5 */
  HT_NEAR(tsys_mispredict_penalty(23.5, 10, 12), 25.0, 1e-9);
  HT_NEAR(tsys_mispredict_penalty(11.0, 10, 12), 0.0, 1e-9);
  /* pattern: pays 11 + rate * 25 ; random reference as above */
  HT_NEAR(tsys_pattern_rate(11.0, 10, 12, 23.5, 10, 12), 0.0, 1e-9);
  HT_NEAR(tsys_pattern_rate(23.5, 10, 12, 23.5, 10, 12), 0.5, 1e-9);
  HT_NEAR(tsys_pattern_rate(11 + 0.1 * 25, 10, 12, 23.5, 10, 12), 0.1, 1e-9);
  HT_CHECK(isnan(tsys_pattern_rate(11, 10, 12, 11, 10, 12)));      /* no random-vs-predictable gap: undefined */
}

HTEST(tsys_linkstack_fit)
{
  static const double D[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16 };
  double y[11], yl[11];
  xts_rng_t r;
  xts_rng_seed(&r, 3);
  for (int i = 0; i < 11; i++) {
    double noise = ((double)xts_rng_below(&r, 1000) - 500) * 1e-3;
    y[i] = 20 + 15 * D[i] + 40 * (D[i] > 4 ? D[i] - 4 : 0) + noise;
    yl[i] = 20 + 15 * D[i] + noise;
  }
  tsys_ls_t f;
  HT_EQ_U64(tsys_fit_linkstack(D, y, 11, &f), 0);
  HT_CHECK(f.ok && f.jump);
  HT_EQ_U64(f.S, 4);
  HT_CHECK(f.lo <= 4 && f.hi >= 4 && f.hi - f.lo <= 1);
  HT_NEAR(f.c, 15, 0.2);
  HT_NEAR(f.M, 40, 0.5);
  HT_EQ_U64(tsys_fit_linkstack(D, yl, 11, &f), 0);
  HT_CHECK(f.ok && !f.jump);                                       /* pure linear: no knee */
  HT_NEAR(f.per_level_lin, 15, 0.1);
  HT_CHECK(tsys_fit_linkstack(D, y, 4, &f) < 0);                   /* too few points */
}

HTEST(tsys_rtc_rate_snap)
{
  double e;
  HT_NEAR(tsys_rtc_snap_rate(1000.7, &e), 1000, 0);
  HT_NEAR(tsys_rtc_snap_rate(990, &e), 1000, 0);
  HT_NEAR(tsys_rtc_snap_rate(32700, &e), 32768, 0);
  HT_NEAR(tsys_rtc_snap_rate(1.01, &e), 1, 0);
  HT_NEAR(tsys_rtc_snap_rate(1030, &e), 1024, 0);
  HT_NEAR(tsys_rtc_snap_rate(500, &e), 0, 0);                      /* no known rate within 5 % */
  HT_NEAR(tsys_rtc_snap_rate(0, &e), 0, 0);
}

HTEST(tsys_cpt_decision)
{
  tsys_cptd_t d;
  /* the measured full-speed numbers of the old runs (dependent 2.04, independent 0.98 cycles at 64): agree, confirm 64 */
  tsys_cpt_decide(2.04 / 64, 0.98 / 64, 64, &d);
  HT_CHECK(d.consistent && !d.set);
  HT_NEAR(d.new_cpt, 64, 0);
  HT_NEAR(d.est, 64.0, 1.0);
  HT_NEAR(d.ratio, 2.08, 0.01);
  /* a slower core: 32 cycles per tick */
  tsys_cpt_decide(2.0 / 32, 1.0 / 32, 64, &d);
  HT_CHECK(d.consistent && d.set);
  HT_NEAR(d.new_cpt, 32, 0);
  /* within 3 % of an integer snaps to it (the hardware ratio is an integer divider); far from one it is kept */
  tsys_cpt_decide(2.0 / 40.5, 1.0 / 40.5, 64, &d);
  HT_CHECK(d.consistent && d.set);
  HT_NEAR(d.new_cpt, 41, 0);
  tsys_cpt_decide(2.0 / 10.5, 1.0 / 10.5, 64, &d);
  HT_CHECK(d.consistent && d.set);
  HT_NEAR(d.new_cpt, 10.5, 1e-9);
  /* inconsistent: ratio 3 (e.g. a broken unit), estimates apart -> never set */
  tsys_cpt_decide(3.0 / 64, 1.0 / 64, 64, &d);
  HT_CHECK(!d.consistent && !d.set);
  HT_NEAR(d.new_cpt, 64, 0);
  tsys_cpt_decide(2.0 / 64, 1.0 / 90, 64, &d);                     /* ratio 2.8 */
  HT_CHECK(!d.consistent && !d.set);
  tsys_cpt_decide(2.0 / 100, 1.0 / 90, 64, &d);                    /* ratio ok-ish, estimates 100 vs 90 */
  HT_CHECK(!d.consistent || d.est_dep - d.est_ind < 0.08 * d.est);
  tsys_cpt_decide(0, 1.0 / 64, 64, &d);                            /* no slope */
  HT_CHECK(!d.consistent && !d.set);
}

static int ids_unique(const xts_caselist_t *cl)
{
  for (int i = 0; i < cl->n; i++)
    for (int j = i + 1; j < cl->n; j++)
      if (!strcmp(cl->v[i].id, cl->v[j].id)) return 0;
  return 1;
}

HTEST(tsys_case_lists)
{
  const char *fam[] = { "branch", "sysops", "calib" };
  int minc[] = { 100, 50, 4 };
  for (int f = 0; f < 3; f++) {
    const xts_family_t *fa = xts_family_find(fam[f]);
    HT_CHECK(fa != NULL);
    if (!fa) continue;
    xts_caselist_t cl = { 0 };
    HT_EQ_U64(fa->enumerate(&cl), 0);
    HT_CHECK(cl.n >= minc[f]);
    HT_CHECK(ids_unique(&cl));
    for (int i = 0; i < cl.n; i++) HT_CHECK(strlen(cl.v[i].id) < 47);
    free(cl.v);
  }
  HT_CHECK(xts_family_has_axis(xts_family_find("branch"), "code_align"));
  HT_CHECK(xts_family_has_axis(xts_family_find("calib"), "speed"));
  HT_CHECK(!strcmp(xts_family_find("calib")->kind, "calib"));
  HT_CHECK(!strcmp(xts_family_find("sysops")->kind, "sched"));
}

/* tools/xts_merge.py has a synthetic-data self test (dedupe, truncation, host refusal, identity); run it when python3 is there */
HTEST(tsys_merge_tool_selftest)
{
  int rc = system("python3 -I tools/xts_merge.py --selftest > /dev/null 2>&1");
  if (rc == -1 || (WIFEXITED(rc) && WEXITSTATUS(rc) == 127)) return;            /* no python3 / not run from the repo root */
  HT_CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
}
