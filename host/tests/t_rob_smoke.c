/* Robustness: the first-boot profiles stay small. smoke (dry run, forked boot) writes fewer than 500 records and never
 * runs self.crash; smoke_crash completes on the PC (its crash case is a skip without the crash simulation).
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

static int dry(const char *root, const xts_profile_t *p)
{
  fflush(NULL);                                        /* no duplicated stdout in children that exit() */
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = { "x", "--root", (char *)root, "--quiet", NULL };
    xts_plat_init(4, argv);
    xts_run_opts_t o = { 0 };
    o.dryrun = 1;
    memset(&xts_dry_stats, 0, sizeof xts_dry_stats);   /* earlier in-process tests count bad words on purpose */
    _exit(xts_run(p, &o));
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *prep(char *root, size_t n, const char *name)
{
  char cmd[700];
  snprintf(root, n, "%s/%s", ht_tmp, name);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  if (system(cmd) != 0) return NULL;
  return root;
}

HTEST(rob_smoke_small)
{
  char root[512], p[700];
  prep(root, sizeof root, "rob_smoke");
  HT_EQ_U64(dry(root, &xts_rob_smoke_profile), 0);
  snprintf(p, sizeof p, "%s/xts/host/0001/results.jsonl", root);
  const char *r = ht_slurp(p);
  int n = r ? ht_count_substr(r, "\n") : 0;
  printf("    smoke dry run: %d records\n", n);
  HT_CHECK(n > 50 && n < 500);
  HT_CHECK(r && !strstr(r, "\"family\":\"self.crash\""));
  HT_CHECK(r && strstr(r, "\"family\":\"self.exec\"") && strstr(r, "\"family\":\"exec.ppc\""));
  snprintf(p, sizeof p, "%s/xts/host/0001/journal.txt", root);
  r = ht_slurp(p);
  HT_CHECK(r && strstr(r, "DONE"));
}

HTEST(rob_smoke_crash_dryrun)
{
  char root[512], p[700];
  prep(root, sizeof root, "rob_smoke_crash");
  HT_EQ_U64(dry(root, &xts_rob_smoke_crash_profile), 0);
  snprintf(p, sizeof p, "%s/xts/host/0001/results.csv", root);
  const char *r = ht_slurp(p);
  HT_CHECK(r && ht_count_substr(r, ",self.crash,ok") == 3 && strstr(r, ",self.crash,crash,"));
  HT_CHECK(r && ht_count_substr(r, "\n") < 200);
  snprintf(p, sizeof p, "%s/xts/host/0001/journal.txt", root);
  r = ht_slurp(p);
  HT_CHECK(r && strstr(r, "END r3:self.crash:-:c0") && strstr(r, "DONE") && !strstr(r, "CRASH "));
}
