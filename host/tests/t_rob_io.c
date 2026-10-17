/* Robustness: USB write failures halt with the FAT32 message (read-only output), write errors during a run are counted
 * (summary.json "write_errors"), and log lines from before xts_sink_open reach run.log. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

static int io_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 6; i++) xts_case_addf(cl, "c%d", i);
  return 0;
}
static int io_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  xts_emit_value(ctx, "index", ctx->case_index, "", XTS_ST_PASS);
  (void)c;
  return 0;
}
XTS_FAMILY(t_robio, .kind = "test", .enumerate = io_enum, .run = io_run, .block_cases = 3);
static const xts_profile_row_t io_rows[] = { { .families = "t.robio" } };
static const xts_profile_t io_prof = { "trobio", "io test", io_rows, 1 };

/* returns the exit status; the child's stderr goes to <root>.err */
static int boot(const char *root)
{
  char err[600];
  snprintf(err, sizeof err, "%s.err", root);
  fflush(NULL);                                        /* no duplicated stdout in children that exit() */
  pid_t pid = fork();
  if (pid == 0) {
    int fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 2); close(fd); }
    char *argv[] = { "x", "--root", (char *)root, "--quiet", NULL };
    xts_plat_init(4, argv);
    _exit(xts_run(&io_prof, NULL) == 0 ? 0 : 3);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void fresh(char *root, size_t n, const char *name)
{
  char cmd[700];
  snprintf(root, n, "%s/%s", ht_tmp, name);
  snprintf(cmd, sizeof cmd, "chmod -R u+w '%s' 2>/dev/null; rm -rf '%s' '%s.err'", root, root, root);
  HT_EQ_U64(system(cmd), 0);
}

HTEST(rob_write_failure_halts)
{
  if (geteuid() == 0) { HT_CHECK(1); return; }       /* root ignores the permission bits */
  char root[512], p[700];
  /* the output root itself is read-only */
  fresh(root, sizeof root, "rob_ro_root");
  HT_EQ_U64(mkdir(root, 0555), 0);
  HT_EQ_U64(boot(root), 1);                            /* host xts_plat_halt: exit(1) */
  snprintf(p, sizeof p, "%s.err", root);
  const char *e = ht_slurp(p);
  HT_CHECK(e && strstr(e, "FAT32 (MBR) USB stick required; write test failed: cannot create"));
  /* a first run works; then the console directory becomes read-only: the next run cannot create its directory */
  fresh(root, sizeof root, "rob_ro_tag");
  HT_EQ_U64(boot(root), 0);
  snprintf(p, sizeof p, "%s/xts/host", root);
  HT_EQ_U64(chmod(p, 0555), 0);
  HT_EQ_U64(boot(root), 1);
  snprintf(p, sizeof p, "%s.err", root);
  e = ht_slurp(p);
  HT_CHECK(e && strstr(e, "write test failed: cannot create") && strstr(e, "xts/host/0002"));
  snprintf(p, sizeof p, "%s/xts/host", root);
  chmod(p, 0755);
}

/* a sink file that cannot be opened (a directory in its place): the run completes and counts the errors */
HTEST(rob_write_errors_counted)
{
  char root[512], p[700];
  fresh(root, sizeof root, "rob_werr");
  snprintf(p, sizeof p, "%s/xts/host/0001/failures.jsonl/x", root);
  HT_EQ_U64(xts_mkdirs(p), 0);
  HT_EQ_U64(boot(root), 0);
  snprintf(p, sizeof p, "%s/xts/host/0001/summary.json", root);
  const char *s = ht_slurp(p);
  HT_CHECK(s && strstr(s, "\"write_errors\":") && !strstr(s, "\"write_errors\":0,"));
  snprintf(p, sizeof p, "%s/xts/host/0001/run.log", root);
  s = ht_slurp(p);
  HT_CHECK(s && strstr(s, "USB write error #1: open") && strstr(s, "write error(s)"));
  snprintf(p, sizeof p, "%s/xts/host/0001/results.csv", root);
  s = ht_slurp(p);
  HT_EQ_U64(ht_count_substr(s, ",pass\n"), 6);
}

/* xts_log before the sink is open: kept and written into run.log at the first flush */
HTEST(rob_early_log_buffered)
{
  char dir[600];
  snprintf(dir, sizeof dir, "%s/rob_earlylog", ht_tmp);
  char cmd[700];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  HT_EQ_U64(system(cmd), 0);
  HT_EQ_U64(xts_mkdirs(dir), 0);
  xts_sink_discard();
  xts_log("early-line-before-open %d", 42);
  HT_EQ_U64(xts_sink_open(dir, "early", 0), 0);
  xts_log("line-after-open");
  xts_sink_flush();
  xts_sink_close();
  snprintf(cmd, sizeof cmd, "%s/run.log", dir);
  const char *log = ht_slurp(cmd);
  HT_CHECK(log && strstr(log, "early-line-before-open 42") && strstr(log, "line-after-open"));
  HT_CHECK(log && strstr(log, "early-line-before-open") < strstr(log, "line-after-open"));
  /* and through a whole run: the registry/startup lines logged before the run directory exists are in run.log */
  char root[512];
  fresh(root, sizeof root, "rob_earlyrun");
  HT_EQ_U64(boot(root), 0);
  snprintf(cmd, sizeof cmd, "%s/xts/host/0001/run.log", root);
  log = ht_slurp(cmd);
  HT_CHECK(log && strstr(log, "watchdog: OFF") && strstr(log, "crash page: survived="));
}

/* a record with a ~15 KB detail (the timing emitter allows 16 KB) is written whole and read back intact */
static char big_detail[15400];
static int big_seen, big_ok;
static int big_cb(const char *line, void *ud)
{
  (void)ud;
  big_seen++;
  big_ok = strstr(line, big_detail) != NULL && !strstr(line, "\"truncated\"") && strstr(line, "\"case_id\":\"big#0\"");
  return 0;
}

HTEST(rob_big_detail_roundtrip)
{
  char dir[600], cmd[700];
  snprintf(dir, sizeof dir, "%s/rob_bigdetail", ht_tmp);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  HT_EQ_U64(system(cmd), 0);
  HT_EQ_U64(xts_mkdirs(dir), 0);
  size_t o = 0;
  o += (size_t)snprintf(big_detail + o, sizeof big_detail - o, "{\"curve\":[");
  for (int i = 0; o + 16 < sizeof big_detail - 8; i++) o += (size_t)snprintf(big_detail + o, sizeof big_detail - o, "%s%d", i ? "," : "", 100000 + i);
  snprintf(big_detail + o, sizeof big_detail - o, "]}");
  HT_CHECK(strlen(big_detail) > 15000);
  xts_sink_discard();
  HT_EQ_U64(xts_sink_open(dir, "big", 0), 0);
  static const xts_family_t fam = { .name = "rob_big", .kind = "timing" };
  xts_case_t c = { .id = "big#0", .insn = -1 };
  xts_ctx_t ctx = { .fam = &fam, .c = &c, .block_key = "r0:rob.big:-:c0" };
  xts_record_t r;
  xts_record_init(&r);
  r.detail = big_detail;
  xts_emit(&ctx, &r);
  big_seen = big_ok = 0;
  HT_EQ_U64(xts_results_foreach("rob_big", big_cb, NULL), 1);
  HT_EQ_U64(big_seen, 1);
  HT_CHECK(big_ok);
  xts_sink_close();
}
