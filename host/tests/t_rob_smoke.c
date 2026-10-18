/* Robustness: the first-boot profiles stay small. smoke (dry run through the receiver) sends fewer than 500 records and
 * never runs self.crash; smoke_crash completes on the PC (its crash case is a skip without the crash simulation).
 * The profile sources are compiled in under other names (one xts_profile per ELF otherwise). */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

#define xts_profile xts_rob_smoke_profile
#define xts_profile_rows_ xts_rob_smoke_rows
#include "../../profiles/smoke.c"
#undef xts_profile
#undef xts_profile_rows_
#define xts_profile xts_rob_smoke_crash_profile
#define xts_profile_rows_ xts_rob_smoke_crash_rows
#include "../../profiles/smoke_crash.c"
#undef xts_profile
#undef xts_profile_rows_

static int dry_child(void *ud)
{
  xts_run_opts_t o = { 0 };
  o.dryrun = 1;
  memset(&xts_dry_stats, 0, sizeof xts_dry_stats);   /* earlier in-process tests count bad words on purpose */
  return xts_run((const xts_profile_t *)ud, &o);
}

HTEST(rob_smoke_small)
{
  char rd[700];
  int rrc = -1;
  HT_EQ_U64(ht_recv_run("rob_smoke", dry_child, (void *)&xts_rob_smoke_profile, NULL, rd, sizeof rd, &rrc), 0);
  HT_EQ_U64(rrc, 0);
  const char *r = ht_run_file(rd, "results.jsonl");
  int n = r ? ht_count_substr(r, "\n") : 0;
  printf("    smoke dry run: %d records\n", n);
  HT_CHECK(n > 50 && n < 500);
  HT_CHECK(r && !strstr(r, "\"family\":\"self.crash\""));
  HT_CHECK(r && strstr(r, "\"family\":\"self.exec\"") && strstr(r, "\"family\":\"exec.ppc\""));
  r = ht_run_file(rd, "receive.json");
  HT_CHECK(r && strstr(r, "\"complete\": true") && strstr(r, "\"status\": \"complete\""));
}

HTEST(rob_smoke_crash_dryrun)
{
  char rd[700];
  int rrc = -1;
  HT_EQ_U64(ht_recv_run("rob_smoke_crash", dry_child, (void *)&xts_rob_smoke_crash_profile, NULL, rd, sizeof rd, &rrc), 0);
  HT_EQ_U64(rrc, 0);
  const char *r = ht_run_file(rd, "results.csv");
  HT_CHECK(r && ht_count_substr(r, ",self.crash,ok") == 3 && strstr(r, ",self.crash,crash,"));
  HT_CHECK(r && ht_count_substr(r, "\n") < 200);
  r = ht_run_file(rd, "receive.json");
  HT_CHECK(r && strstr(r, "\"status\": \"complete\"") && strstr(r, "\"BLOCK\":"));
  HT_CHECK(ht_run_file(rd, "crash.json") == NULL);
}
