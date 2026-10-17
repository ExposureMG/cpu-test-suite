/* journal resume: a block hangs (or crashes) mid-way, the next boot records it and finishes the run.
 * Each "boot" is a forked child running the real xts_run() on a host root directory.
 * Without the crash page (lostpage) the case is unknown: boot 2 re-runs the block with CASE lines (RETRY) and hangs in
 * the same case again, so boot 3 knows it from the journal, quarantines only that case and finishes. */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

static int die_at = -1, die_crash;
static int tr_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 20; i++) xts_case_addf(cl, "c%02d", i);
  return 0;
}
static int tr_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (ctx->case_index == die_at) {
    if (die_crash) {
      uint64_t fake[40] = { 0 };
      fake[36] = 0x80001234;
      fake[37] = 0x9000000000003030ull;
      xts_crash_record(0x300, fake);
    }
    _exit(9);                                    /* power cut / hang: nothing after this point happens */
  }
  xts_emit_value(ctx, "index", ctx->case_index, "", XTS_ST_PASS);
  (void)c;
  return 0;
}
XTS_FAMILY(t_resume, .kind = "test", .enumerate = tr_enum, .run = tr_run, .block_cases = 5);

static const xts_profile_row_t rows[] = { { .families = "t.resume" } };
static const xts_profile_t prof = { "tresume", "resume test", rows, 1 };

static int boot(const char *root, int die, int crash)
{
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = { "x", "--root", (char *)root, "--quiet", NULL };
    xts_plat_init(4, argv);
    die_at = die;
    die_crash = crash;
    _exit(xts_run(&prof, NULL) == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void scenario(const char *name, int crash, int lose_page)
{
  char root[512], cmd[600], path[700];
  snprintf(root, sizeof root, "%s/%s", ht_tmp, name);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  HT_EQ_U64(system(cmd), 0);
  HT_EQ_U64(boot(root, 7, crash), 9);            /* boot 1 dies in case 7 (block c1 = cases 5..9) */
  snprintf(path, sizeof path, "%s/xts/host/current.txt", root);
  HT_CHECK(ht_slurp(path) != NULL);
  snprintf(path, sizeof path, "%s/xts/host/0001/journal.txt", root);
  const char *j = ht_slurp(path);
  HT_CHECK(j && strstr(j, "BEGIN r0:t.resume:-:c1") && !strstr(j, "END r0:t.resume:-:c1"));
  if (lose_page) {
    snprintf(path, sizeof path, "%s/.xts_crashpage.bin", root);
    unlink(path);
    HT_EQ_U64(boot(root, 7, 0), 9);              /* boot 2: case unknown -> RETRY, per-case re-run, same hang */
    unlink(path);
    snprintf(path, sizeof path, "%s/xts/host/0001/journal.txt", root);
    j = ht_slurp(path);
    HT_CHECK(j && strstr(j, "RETRY r0:t.resume:-:c1") && strstr(j, "CASE r0:t.resume:-:c1 2\n") &&
             !strstr(j, "CASE r0:t.resume:-:c1 3\n"));
  }
  HT_EQ_U64(boot(root, -1, 0), 0);               /* boot 2 (3) resumes */
  snprintf(path, sizeof path, "%s/xts/host/0001/journal.txt", root);
  j = ht_slurp(path);
  HT_CHECK(j && strstr(j, lose_page ? "BOOT 3" : "BOOT 2") && strstr(j, "DONE"));
  if (lose_page) HT_CHECK(j && strstr(j, "QUAR r0:t.resume:-:c1 case=2 status=hang") && !strstr(j, "case=-1"));
  else HT_CHECK(j && strstr(j, crash ? "QUAR r0:t.resume:-:c1 case=2 status=crash" : "QUAR r0:t.resume:-:c1 case=2 status=hang"));
  snprintf(path, sizeof path, "%s/xts/host/0001/results.csv", root);
  const char *csv = ht_slurp(path);
  HT_CHECK(csv != NULL);
  if (!csv) return;
  for (int i = 0; i < 20; i++) {                 /* every case once: 19 pass, c07 as the hang/crash record */
    char needle[64];
    snprintf(needle, sizeof needle, ",t.resume,c%02d,", i);
    HT_EQ_U64(ht_count_substr(csv, needle), 1);
  }
  HT_EQ_U64(ht_count_substr(csv, ",t.resume,*,"), lose_page ? 1 : 0);   /* the unknown-case hang that led to RETRY */
  HT_EQ_U64(ht_count_substr(csv, crash ? ",crash\n" : ",hang\n"), lose_page ? 2 : 1);
  HT_EQ_U64(ht_count_substr(csv, ",pass\n"), 19);
  snprintf(path, sizeof path, "%s/xts/host/current.txt", root);
  HT_CHECK(ht_slurp(path) == NULL);
  snprintf(path, sizeof path, "%s/xts/host/0001/summary.json", root);
  const char *sum = ht_slurp(path);
  HT_CHECK(sum && strstr(sum, "\"t.resume\""));
  snprintf(path, sizeof path, "%s/xts/host/0001/failures.jsonl", root);
  const char *fl = ht_slurp(path);
  HT_CHECK(fl && ht_count_substr(fl, "\n") == (lose_page ? 2 : 1));
  if (crash) HT_CHECK(fl && strstr(fl, "\"vector\":\"0300\"") && strstr(fl, "\"srr0\":\"0000000080001234\""));
  snprintf(path, sizeof path, "%s/xts/host/0001/sysinfo.json", root);
  HT_CHECK(ht_slurp(path) != NULL);
  /* a third boot starts a NEW run (the previous one is complete) */
  HT_EQ_U64(boot(root, -1, 0), 0);
  snprintf(path, sizeof path, "%s/xts/host/0002/journal.txt", root);
  j = ht_slurp(path);
  HT_CHECK(j && strstr(j, "START tresume") && strstr(j, "DONE"));
}

HTEST(resume_hang) { scenario("hang", 0, 0); }
HTEST(resume_crash) { scenario("crash", 1, 0); }
HTEST(resume_no_crash_page) { scenario("lostpage", 0, 1); }
