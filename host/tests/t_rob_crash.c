/* Robustness: the crash path's crash.json, progress.bin naming the case without a report, per-case re-run naming, the
 * opt-in self.crash family, and resume equivalence with the persistent (fsync'ed) sinks. Every "boot" is a forked child
 * running the real xts_run() on a host root; a crash is simulated through xts_plat_self_crash (xts_host_crash_sim). */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

static int rc_crash_at = -1, rc_hang_at = -1;
static int rc_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 20; i++) xts_case_addf(cl, "c%02d", i);
  return 0;
}
static int rc_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (ctx->case_index == rc_crash_at) xts_plat_self_crash();   /* simulated: crash.json, _exit(9) */
  if (ctx->case_index == rc_hang_at) _exit(9);                 /* hang / power cut */
  xts_emit_value(ctx, "index", ctx->case_index, "", XTS_ST_PASS);
  (void)c;
  return 0;
}
XTS_FAMILY(t_robcrash, .kind = "test", .enumerate = rc_enum, .run = rc_run, .block_cases = 5);

static const xts_profile_row_t rc_rows[] = { { .families = "t.robcrash" } };
static const xts_profile_t rc_prof = { "trobcrash", "crash path test", rc_rows, 1 };
static const xts_profile_row_t sc_rows[] = { { .families = "t.robcrash", .max_cases = 5 }, { .families = "self.crash" } };
static const xts_profile_t sc_prof = { "trobself", "self.crash test", sc_rows, 2 };
static const xts_profile_row_t glob_rows[] = { { .families = "self.*" } };
static const xts_profile_t glob_prof = { "trobglob", "self.* without self.crash", glob_rows, 1 };

static int boot(const char *root, const xts_profile_t *p, int crash_at, int hang_at, int sim, int max_blocks)
{
  fflush(NULL);                                        /* no duplicated stdout in children that exit() */
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = { "x", "--root", (char *)root, "--quiet", NULL };
    xts_plat_init(4, argv);
    rc_crash_at = crash_at;
    rc_hang_at = hang_at;
    xts_host_crash_sim = sim;
    xts_run_opts_t o = { 0 };
    o.max_blocks = max_blocks;
    _exit(xts_run(p, &o) == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *rd(const char *root, const char *rel)
{
  char path[700];
  snprintf(path, sizeof path, "%s/xts/host/0001/%s", root, rel);
  return ht_slurp(path);
}

static void fresh(char *root, size_t n, const char *name)
{
  char cmd[700];
  snprintf(root, n, "%s/%s", ht_tmp, name);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  HT_EQ_U64(system(cmd), 0);
}

static void drop(const char *root, const char *file)
{
  char path[600];
  snprintf(path, sizeof path, "%s/xts/host/0001/%s", root, file);
  unlink(path);
}

/* crash in case 7 (block c1, k=2): crash.json filled; boot 2 quarantines exactly that case. Without crash.json (the
 * report could not be written) progress.bin still names the case, as a hang. */
static void crash_scenario(const char *name, int keep_report)
{
  char root[512];
  fresh(root, sizeof root, name);
  HT_EQ_U64(boot(root, &rc_prof, 7, -1, 1, 0), 9);
  const char *j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "BEGIN r0:t.robcrash:-:c1\n") && !strstr(j, "END r0:t.robcrash:-:c1") && !strstr(j, "CRASH"));
  const char *cj = rd(root, "crash.json");
  HT_CHECK(cj && strstr(cj, "\"state\":\"crashed\"") && strstr(cj, "\"kind\":\"main\"") && strstr(cj, "\"vector\":\"0700\""));
  HT_CHECK(cj && strstr(cj, "\"block\":\"r0:t.robcrash:-:c1\"") && strstr(cj, "\"case_rel\":2") && strstr(cj, "\"case_id\":\"c07\""));
  HT_CHECK(cj && strstr(cj, "\"outcome\":\"saved\"") && strstr(cj, "\"log_0\":") && strstr(cj, "\"r31\":"));
  if (!keep_report) drop(root, "crash.json");
  HT_EQ_U64(boot(root, &rc_prof, -1, -1, 0, 0), 0);
  j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, keep_report ? "QUAR r0:t.robcrash:-:c1 case=2 status=crash" : "QUAR r0:t.robcrash:-:c1 case=2 status=hang") &&
           strstr(j, "DONE") && !strstr(j, "RETRY"));
  HT_CHECK(j && (keep_report ? strstr(j, "REPORT tb=") != NULL : !strstr(j, "REPORT")));
  const char *fl = rd(root, "failures.jsonl");
  HT_CHECK(fl && ht_count_substr(fl, "\n") == 1 && strstr(fl, "\"case_id\":\"c07\"") &&
           strstr(fl, keep_report ? "\"status\":\"crash\"" : "\"status\":\"hang\""));
  HT_CHECK(fl && strstr(fl, keep_report ? "\"case_from\":\"crash.json\"" : "\"case_from\":\"progress.bin (the case that was running)\""));
  if (keep_report) HT_CHECK(fl && strstr(fl, "\"vector\":\"0700\"") && strstr(fl, "\"crash_report\":{\"state\":\"crashed\""));
  else HT_CHECK(fl && strstr(fl, "\"breadcrumb\":{\"state\":\"case\""));
  const char *csv = rd(root, "results.csv");
  HT_EQ_U64(ht_count_substr(csv, ",pass\n"), 19);
  const char *si = rd(root, "sysinfo_boot2.json");
  HT_CHECK(si && strstr(si, keep_report ? "\"previous_boot\":\"crash.json filled" : "\"previous_boot\":\"crash.json missing; progress.bin: case"));
  if (keep_report) HT_CHECK(rd(root, "crash_boot1.json") != NULL);
  cj = rd(root, "crash.json");                         /* re-armed for the next boot */
  HT_CHECK(cj && strstr(cj, "\"state\":\"armed\"") && strstr(cj, "\"boot\":2"));
}

HTEST(rob_crash_report) { crash_scenario("rob_crash_report", 1); }
HTEST(rob_crash_no_report) { crash_scenario("rob_crash_noreport", 0); }

/* hang without a breadcrumb: boot 2 re-runs the block case by case and hangs in a DIFFERENT case (13); boot 3
 * quarantines that one only (named by progress.bin) and the earlier culprit (12) runs fine */
HTEST(rob_percase_rerun_names_case)
{
  char root[512];
  fresh(root, sizeof root, "rob_percase");
  HT_EQ_U64(boot(root, &rc_prof, -1, 12, 0, 0), 9);
  drop(root, "progress.bin");
  HT_EQ_U64(boot(root, &rc_prof, -1, 13, 0, 0), 9);
  const char *j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "RETRY r0:t.robcrash:-:c2\nBEGIN r0:t.robcrash:-:c2\n"));
  const char *pb = rd(root, "progress.bin");
  HT_CHECK(pb && strstr(pb, "\"case_id\":\"c13\"") && strstr(pb, "\"boot\":2"));
  HT_EQ_U64(boot(root, &rc_prof, -1, -1, 0, 0), 0);
  j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "QUAR r0:t.robcrash:-:c2 case=3 status=hang") && ht_count_substr(j, "QUAR ") == 1 && strstr(j, "DONE"));
  const char *csv = rd(root, "results.csv");
  HT_EQ_U64(ht_count_substr(csv, ",t.robcrash,c12,"), 1);
  HT_EQ_U64(ht_count_substr(csv, ",t.robcrash,c13,"), 1);
  HT_EQ_U64(ht_count_substr(csv, ",t.robcrash,*,"), 1);
  HT_EQ_U64(ht_count_substr(csv, ",hang\n"), 2);
  HT_EQ_U64(ht_count_substr(csv, ",pass\n"), 19);
  const char *si = rd(root, "sysinfo_boot3.json");
  HT_CHECK(si && strstr(si, "progress.bin: case in block r0:t.robcrash:-:c2 case c13"));
}

/* self.crash: opt-in (a self.* row alone gives it no cases); named in a row, its last case crashes through the crash
 * path; the resumed run quarantines it and finishes */
HTEST(rob_self_crash_family)
{
  char root[512];
  fresh(root, sizeof root, "rob_selfglob");
  HT_EQ_U64(boot(root, &glob_prof, -1, -1, 1, 0), 0);
  const char *csv = rd(root, "results.csv");
  HT_CHECK(csv && !strstr(csv, "self.crash"));
  fresh(root, sizeof root, "rob_selfcrash");
  HT_EQ_U64(boot(root, &sc_prof, -1, -1, 1, 0), 9);
  const char *j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "END r0:t.robcrash:-:c0") && strstr(j, "BEGIN r1:self.crash:-:c0") && !strstr(j, "END r1:"));
  const char *cj = rd(root, "crash.json");
  HT_CHECK(cj && strstr(cj, "\"block\":\"r1:self.crash:-:c0\"") && strstr(cj, "\"case_id\":\"crash\"") &&
           strstr(cj, "\"family\":\"self.crash\""));
  HT_EQ_U64(boot(root, &sc_prof, -1, -1, 0, 0), 0);
  j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "QUAR r1:self.crash:-:c0 case=3 status=crash") && strstr(j, "DONE"));
  csv = rd(root, "results.csv");
  HT_EQ_U64(ht_count_substr(csv, ",self.crash,ok"), 3);
  HT_EQ_U64(ht_count_substr(csv, ",self.crash,crash,"), 1);
  HT_EQ_U64(ht_count_substr(csv, ",crash\n"), 1);
  /* without the simulation (PC dry run) the crash case is a skip */
  fresh(root, sizeof root, "rob_selfnosim");
  HT_EQ_U64(boot(root, &sc_prof, -1, -1, 0, 0), 0);
  csv = rd(root, "results.csv");
  HT_CHECK(csv && strstr(csv, ",self.crash,crash,") && ht_count_substr(csv, ",skip\n") == 1);
}

/* persistent sinks: a run split over several boots (max_blocks) writes exactly what one boot writes */
HTEST(rob_resume_equivalence)
{
  char one[512], split[512];
  fresh(one, sizeof one, "rob_eq_one");
  fresh(split, sizeof split, "rob_eq_split");
  HT_EQ_U64(boot(one, &rc_prof, -1, -1, 0, 0), 0);
  for (int i = 0; i < 5; i++) HT_EQ_U64(boot(split, &rc_prof, -1, -1, 0, 1), 0);   /* 4 blocks, then DONE */
  const char *files[] = { "results.jsonl", "results.csv", "failures.jsonl" };
  for (int f = 0; f < 3; f++) {
    char *a = strdup(rd(one, files[f]) ? rd(one, files[f]) : "?");
    const char *b = rd(split, files[f]);
    HT_CHECK(b && !strcmp(a, b));
    free(a);
  }
  const char *j = rd(split, "journal.txt");
  HT_CHECK(j && strstr(j, "BOOT 5") && strstr(j, "DONE") && ht_count_substr(j, "END ") == 4);
  const char *log = rd(one, "run.log");
  HT_CHECK(log && ht_count_substr(log, "flush: block r0:t.robcrash:-:c") >= 3);
  const char *sum = rd(one, "summary.json");
  HT_CHECK(sum && strstr(sum, "\"write_errors\":0") && strstr(sum, "\"flush_ms_total\":"));
}
