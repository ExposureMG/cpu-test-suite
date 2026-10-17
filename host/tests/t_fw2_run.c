/* framework v2 runs (forked boots of the real xts_run): family finish() ordering and resume semantics, a PIR != 0 crash
 * reported on the next boot, max_cases sampling, leaked exec hooks removed after a block. */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

/* ---- t.fin: 6 cases, 2 per block, axis sf=* -> 6 blocks per row, finish after them ---- */
static int fin_die_at = -1, fin_thread_crash, fin_leak_hook;
static int fin_runs_this_boot;
static int fin_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 6; i++) xts_case_addf(cl, "f%d", i);
  return 0;
}
static int leak_pre(xts_exec_t *x, void *ud) { (void)x; (void)ud; return 0; }
static int fin_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (ctx->case_index == fin_die_at && xts_cond_value(ctx, "sf") == 1) {
    if (fin_thread_crash) {
      uint64_t fake[40] = { 0 };
      fake[36] = 0x80007777;
      fake[39] = 3;                              /* PIR 3 crashed while the main thread ran this case */
      xts_crash_record(0x700, fake);
    }
    _exit(9);
  }
  if (fin_leak_hook && ctx->case_index == 0) xts_exec_hook_add(leak_pre, NULL, NULL);   /* never removed */
  fin_runs_this_boot++;
  xts_emit_value(ctx, "count", ctx->case_index, "count", XTS_ST_PASS);
  (void)c;
  return 0;
}
static int count_line(const char *line, void *ud)
{
  if (strstr(line, "\"metric\":\"count\"")) (*(int *)ud)++;
  return 0;
}
static int fin_finish(xts_ctx_t *ctx)
{
  int on_disk = 0;
  xts_results_foreach("t.fin", count_line, &on_disk);
  char det[256];
  snprintf(det, sizeof det, "{\"row\":%d,\"blocks\":%d,\"ran\":%d,\"earlier\":%d,\"quar\":%d,\"skipped\":%d,\"complete\":%d,"
           "\"runs_mem\":%d,\"on_disk\":%d,\"hooks\":%d}",
           ctx->fin->row, ctx->fin->blocks, ctx->fin->ran_this_boot, ctx->fin->done_earlier, ctx->fin->quarantined,
           ctx->fin->skipped, ctx->fin->complete, fin_runs_this_boot, on_disk, xts_exec_hook_count());
  xts_record_t r;
  xts_record_init(&r);
  r.case_id = "summary";
  r.metric = "count";
  r.value = on_disk;
  r.unit = "count";
  r.status = XTS_ST_INFO;
  r.detail = det;
  xts_emit(ctx, &r);
  fin_runs_this_boot = 0;
  return ctx->case_index == -1 && ctx->c && !strcmp(ctx->c->id, "finish") ? 0 : -1;
}
XTS_FAMILY(t_fin, .kind = "test", .axes = "sf", .enumerate = fin_enum, .run = fin_run, .finish = fin_finish, .block_cases = 2);

static const xts_profile_row_t fin_rows[] = { { .families = "t.fin", .axes = "sf=*" }, { .families = "t.fin" } };
static const xts_profile_t fin_prof = { "tfin", "finish test", fin_rows, 2 };

/* ---- t.cap: 100 cases, max_cases 10 -> every 10th ---- */
static int cap_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 100; i++) xts_case_addf(cl, "k%03d", i);
  return 0;
}
static int cap_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  xts_emit_value(ctx, "count", ctx->case_index, "count", XTS_ST_PASS);
  (void)c;
  return 0;
}
XTS_FAMILY(t_cap, .kind = "test", .enumerate = cap_enum, .run = cap_run, .block_cases = 4);
static const xts_profile_row_t cap_rows[] = { { .families = "t.cap", .max_cases = 10 }, { .families = "t.cap", .max_cases = 7 },
                                              { .families = "t.cap", .max_cases = 200 } };
static const xts_profile_t cap_prof = { "tcap", "max_cases test", cap_rows, 3 };

static int boot(const xts_profile_t *prof, const char *root, int die, int thread_crash, int leak)
{
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = { "x", "--root", (char *)root, "--quiet", NULL };
    xts_plat_init(4, argv);
    fin_die_at = die;
    fin_thread_crash = thread_crash;
    fin_leak_hook = leak;
    _exit(xts_run(prof, NULL) == 0 ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int pos_of(const char *hay, const char *needle) { const char *p = strstr(hay, needle); return p ? (int)(p - hay) : -1; }
static int last_pos_of(const char *hay, const char *needle)
{
  int last = -1;
  for (const char *p = hay; (p = strstr(p, needle)); p++) last = (int)(p - hay);
  return last;
}

HTEST(fw2_finish_order)
{
  char root[512], cmd[600], path[700];
  snprintf(root, sizeof root, "%s/fin1", ht_tmp);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  HT_EQ_U64(system(cmd), 0);
  HT_EQ_U64(boot(&fin_prof, root, -1, 0, 1), 0);
  snprintf(path, sizeof path, "%s/xts/host/0001/results.jsonl", root);
  const char *js = ht_slurp(path);
  HT_CHECK(js != NULL);
  if (!js) return;
  char *copy = strdup(js);
  HT_EQ_U64(ht_count_substr(copy, "\"case_id\":\"summary\""), 2);     /* one per row */
  /* row 0: both sf values (12 case records) come before its summary; row 1 (6 more) before the second */
  int s0 = pos_of(copy, "\"case_id\":\"summary\""), s1 = last_pos_of(copy, "\"case_id\":\"summary\"");
  int last_r0 = last_pos_of(copy, "\"block\":\"r0:t.fin:sf=32:c2\""), first_r1 = pos_of(copy, "\"block\":\"r1:t.fin:-:c0\"");
  HT_CHECK(last_r0 >= 0 && last_r0 < s0 && s0 < first_r1 && first_r1 < s1);
  HT_CHECK(strstr(copy, "\"block\":\"r0:t.fin:finish\"") && strstr(copy, "\"block\":\"r1:t.fin:finish\""));
  HT_CHECK(strstr(copy, "\"detail\":{\"row\":0,\"blocks\":6,\"ran\":6,\"earlier\":0,\"quar\":0,\"skipped\":0,\"complete\":1,"
                        "\"runs_mem\":12,\"on_disk\":12,\"hooks\":0}") != NULL);
  HT_CHECK(strstr(copy, "\"detail\":{\"row\":1,\"blocks\":3,\"ran\":3,\"earlier\":0,\"quar\":0,\"skipped\":0,\"complete\":1,"
                        "\"runs_mem\":6,\"on_disk\":19,\"hooks\":0}") != NULL);   /* 18 case records + row 0's summary */
  HT_CHECK(strstr(copy, "\"case_id\":\"summary\",\"mnem\":\"\",\"word\":\"\",\"cond\":{") != NULL);
  free(copy);
  snprintf(path, sizeof path, "%s/xts/host/0001/journal.txt", root);
  const char *jl = ht_slurp(path);
  HT_CHECK(jl && ht_count_substr(jl, "BEGIN r0:t.fin:finish") == 1 && ht_count_substr(jl, "END r0:t.fin:finish") == 1);
  /* the leaked hook was removed after its block, and logged */
  snprintf(path, sizeof path, "%s/xts/host/0001/run.log", root);
  const char *lg = ht_slurp(path);
  HT_CHECK(lg && strstr(lg, "exec hook(s) still registered after the axes were reverted: removed") != NULL);
}

HTEST(fw2_finish_resume)
{
  char root[512], cmd[600], path[700];
  snprintf(root, sizeof root, "%s/fin2", ht_tmp);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  HT_EQ_U64(system(cmd), 0);
  /* boot 1 dies in case 3 of the sf=32 combination (block r0:...:sf=32:c1), after PIR 3 crashed */
  HT_EQ_U64(boot(&fin_prof, root, 3, 1, 0), 9);
  HT_EQ_U64(boot(&fin_prof, root, -1, 0, 0), 0);
  snprintf(path, sizeof path, "%s/xts/host/0001/results.jsonl", root);
  const char *js = ht_slurp(path);
  HT_CHECK(js != NULL);
  if (!js) return;
  char *copy = strdup(js);
  HT_EQ_U64(ht_count_substr(copy, "\"case_id\":\"summary\""), 2);
  /* row 0 finish after the resume: 4 blocks done in boot 1, the hung one re-run in boot 2 without its case 3
   * (quarantined), the last one run in boot 2 */
  HT_CHECK(strstr(copy, "\"detail\":{\"row\":0,\"blocks\":6,\"ran\":2,\"earlier\":4,\"quar\":1,\"skipped\":0,\"complete\":0,") != NULL);
  HT_CHECK(strstr(copy, "\"status\":\"hang\"") != NULL);
  /* the PIR 3 crash from boot 1 is a crash record of the block the main thread was in */
  HT_CHECK(strstr(copy, "\"case_id\":\"pir3\"") != NULL);
  int p = pos_of(copy, "\"case_id\":\"pir3\"");
  HT_CHECK(p >= 0 && strstr(copy + p, "\"block\":\"r0:t.fin:sf=32:c1\"") != NULL && strstr(copy + p, "\"status\":\"crash\"") != NULL);
  HT_CHECK(strstr(copy, "\"srr0\":\"0000000080007777\"") != NULL);
  free(copy);
  snprintf(path, sizeof path, "%s/xts/host/0001/journal.txt", root);
  const char *jl = ht_slurp(path);
  HT_CHECK(jl && ht_count_substr(jl, "BEGIN r0:t.fin:finish") == 1 && strstr(jl, "QUAR r0:t.fin:sf=32:c1 case=1 status=hang"));
}

HTEST(fw2_max_cases)
{
  /* plan: stride ceil(100/10) = 10 -> k000 k010 ... k090 in blocks of 4; 100/7 -> stride 15, 7 cases; 200 -> all */
  xts_plan_t p;
  HT_EQ_U64(xts_plan_build(&p, &cap_prof), 0);
  int n0 = 0, n1 = 0, n2 = 0;
  for (int i = 0; i < p.n; i++) {
    const xts_block_t *b = &p.b[i];
    if (b->row == 0) { n0 += b->ncases; HT_EQ_U64(b->stride, 10); }
    if (b->row == 1) { n1 += b->ncases; HT_EQ_U64(b->stride, 15); }
    if (b->row == 2) { n2 += b->ncases; HT_EQ_U64(b->stride, 1); }
  }
  HT_EQ_U64(n0, 10);
  HT_EQ_U64(n1, 7);
  HT_EQ_U64(n2, 100);
  HT_CHECK(p.n >= 3 && p.b[0].first_case == 0 && p.b[0].ncases == 4 && p.b[1].first_case == 40 && p.b[2].first_case == 80 &&
           p.b[2].ncases == 2);
  HT_EQ_U64(xts_block_case(&p.b[1], 3), 70);
  HT_EQ_U64(xts_block_rel(&p.b[1], 70), 3);
  HT_EQ_U64(xts_block_rel(&p.b[1], 75), (uint64_t)-1);
  HT_EQ_U64(xts_block_rel(&p.b[1], 80), (uint64_t)-1);
  HT_CHECK(!strcmp(p.b[0].key, "r0:t.cap:-:c0") && !strcmp(p.b[2].key, "r0:t.cap:-:c2"));
  char root[512], cmd[600], path[700];
  snprintf(root, sizeof root, "%s/cap", ht_tmp);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  HT_EQ_U64(system(cmd), 0);
  HT_EQ_U64(boot(&cap_prof, root, -1, 0, 0), 0);
  snprintf(path, sizeof path, "%s/xts/host/0001/results.jsonl", root);
  const char *js = ht_slurp(path);
  HT_CHECK(js != NULL);
  if (!js) return;
  HT_EQ_U64(ht_count_substr(js, "\"family\":\"t.cap\""), 117);
  HT_EQ_U64(ht_count_substr(js, "\"case_id\":\"k090\""), 3);          /* rows 0 (stride 10), 1 (stride 15: 90) and 2 */
  HT_EQ_U64(ht_count_substr(js, "\"case_id\":\"k095\""), 1);          /* only row 2 */
  HT_EQ_U64(ht_count_substr(js, "\"case_id\":\"k015\""), 2);          /* rows 1 and 2 */
}
