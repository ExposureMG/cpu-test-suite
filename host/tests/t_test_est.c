/* test_est: exec.est enumeration (every estimate op, sweep ids, chunking) and a full host run through the real scheduler /
 * sinks (inputs recorded, tininess probe records, no failures, record budget) */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>        /* xts_run (framework test) */
#include "htest.h"

static int est_enum(xts_caselist_t *cl)
{
  const xts_family_t *f = xts_family_find("exec_est");
  if (!f) return -1;
  memset(cl, 0, sizeof *cl);
  return f->enumerate(cl);
}

HTEST(test_est_enumerate)
{
  xts_caselist_t cl;
  const xts_family_t *f = xts_family_find("exec_est");
  HT_CHECK(f != NULL);
  if (!f) return;
  HT_CHECK(!strcmp(f->kind, "semantic") && !strcmp(f->axes, "sf,vscr_nj"));
  HT_CHECK(!strcmp(xts_family_name(f), "exec.est"));
  HT_EQ_U64(est_enum(&cl), 0);
  HT_CHECK(cl.n > 400 && cl.n < 3000);
  static const int ops[] = { XI_fresx, XI_frsqrtex, XI_vrefp, XI_vrsqrtefp, XI_vexptefp, XI_vlogefp, XI_vrefp128,
                             XI_vrsqrtefp128, XI_vexptefp128, XI_vlogefp128 };
  for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
    int n = 0, edge = 0, main_ = 0;
    for (int i = 0; i < cl.n; i++)
      if (cl.v[i].insn == ops[k]) {
        n++;
        edge += strstr(cl.v[i].id, ".edge#") != NULL;
        main_ += strstr(cl.v[i].id, ".main#") != NULL;
      }
    HT_CHECK(n > 0 && edge >= 1 && main_ > 0);
  }
  long runs = 0;
  int tiny = 0;
  for (int i = 0; i < cl.n; i++) {
    for (int j = i + 1; j < cl.n; j++)
      if (!strcmp(cl.v[i].id, cl.v[j].id)) { HT_CHECK(0); printf("      duplicate id %s\n", cl.v[i].id); }
    if (cl.v[i].arg[1] == 6) tiny++;
    else {
      HT_CHECK(cl.v[i].arg[3] >= 1 && cl.v[i].arg[3] <= 32);
      runs += cl.v[i].arg[3];
    }
  }
  HT_EQ_U64(tiny, 4);
  HT_CHECK(runs > 15000 && runs < 60000);
  /* recip main sweep: 4096 bins x 3 patterns / 4 lanes = 3072 runs = 96 chunks; 128 form: 4096/16 */
  int vrefp_main = 0, vrefp128_main = 0;
  for (int i = 0; i < cl.n; i++) {
    vrefp_main += !strncmp(cl.v[i].id, "vrefp.main#", 11);
    vrefp128_main += !strncmp(cl.v[i].id, "vrefp128.main#", 14);
  }
  HT_EQ_U64(vrefp_main, 96);
  HT_EQ_U64(vrefp128_main, 6);
}

static const xts_profile_row_t rows[] = { { .families = "exec.est" } };
static const xts_profile_t prof = { "testest", "exec.est host run", rows, 1 };

static int est_child(void *ud)
{
  (void)ud;
  return xts_run(&prof, NULL) == 0 ? 0 : 1;
}

HTEST(test_est_host_run)
{
  char rd[700];
  int rrc = -1;
  HT_EQ_U64(ht_recv_run("est", est_child, NULL, NULL, rd, sizeof rd, &rrc), 0);
  HT_EQ_U64(rrc, 0);                                   /* > 15000 records over localhost UDP without a loss */
  const char *js = ht_run_file(rd, "results.jsonl");
  HT_CHECK(js != NULL);
  if (!js) return;
  HT_EQ_U64(ht_count_substr(js, "\"metric\":\"tininess\""), 4);
  HT_EQ_U64(ht_count_substr(js, "\"tininess\":\"before-rounding\""), 4);   /* host: the model itself */
  HT_CHECK(ht_count_substr(js, "\"metric\":\"estimate\"") > 15000);
  HT_CHECK(strstr(js, "\"case_id\":\"vrefp.main#0\"") != NULL);
  HT_CHECK(strstr(js, "\"case_id\":\"fres.edge#0\"") != NULL);
  /* exact input bits are in the detail: the first vrefp lanes are 1.0 with the three low patterns, bin 0 and bin 1 */
  HT_CHECK(strstr(js, "\"i\":[\"3F800000\",\"3F8007FF\",\"3F800400\",\"3F800800\"]") != NULL);
  HT_CHECK(strstr(js, "\"status\":\"fail\"") == NULL);
  HT_CHECK(strstr(js, "\"status\":\"trap\"") == NULL);
  HT_CHECK(strstr(js, "\"truncated\"") == NULL);
  const char *fl = ht_run_file(rd, "failures.jsonl");
  HT_CHECK(fl == NULL || fl[0] == 0);
}
