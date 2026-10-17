/* Crash reports and the write guard, host directory mode: the sector packer / squeeze (crash.json stays valid JSON
 * whatever sectors are filled), thread slots, a family that tries to write files while its cases run (refused, counted,
 * the run goes on), and a block whose records outgrow the buffer (chunk written at a case boundary, PART line; a re-run
 * after a hang starts after the written chunk, nothing is duplicated). */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <core/core.h>
#include "htest.h"

static const char *py(void) { return getenv("PYTHON") ? getenv("PYTHON") : "python3"; }

/* python json.load on a file: 0 ok */
static int json_ok(const char *path)
{
  char cmd[900];
  snprintf(cmd, sizeof cmd, "%s -I -c 'import json,sys; json.load(open(sys.argv[1]))' '%s' 2>/dev/null", py(), path);
  return system(cmd) == 0 ? 0 : -1;
}

HTEST(crashrep_pack_squeeze)
{
  static uint8_t img[4 * 512];
  const char *magic = "XTS1abcdef012345";
  xts_spack_t p;
  xts_spack_init(&p, img, 512, 4, magic);
  HT_EQ_U64(memcmp(img + XTS_SLOT_MAGIC_OFF, magic, XTS_SLOT_MAGIC_LEN), 0);
  HT_EQ_U64(memcmp(img + 3 * 512 + XTS_SLOT_MAGIC_OFF, magic, XTS_SLOT_MAGIC_LEN), 0);
  HT_EQ_U64(img[0], '{');
  HT_EQ_U64(img[512], ',');
  HT_EQ_U64(img[4 * 512 - 2], '}');
  HT_EQ_U64(img[511], '\n');
  char m[600];
  for (int i = 0; i < 9; i++) {                       /* 9 x ~100 bytes: spills into sector 1 and 2 */
    int n = snprintf(m, sizeof m, ",\"k%d\":\"%090d\"", i, i);
    HT_EQ_U64(xts_spack_put(&p, m, (uint32_t)n), 0);
  }
  HT_EQ_U64(xts_spack_used(&p), 3);
  memset(m, 'x', sizeof m);                           /* larger than a sector's payload: dropped */
  HT_EQ_U64(xts_spack_put(&p, m, 500), (uint64_t)-1);
  HT_EQ_U64(p.dropped, 1);
  for (int s = 0; s < 4; s++) HT_EQ_U64(memcmp(img + s * 512 + XTS_SLOT_MAGIC_OFF, magic, XTS_SLOT_MAGIC_LEN), 0);
  char path[600];
  snprintf(path, sizeof path, "%s/pack.json", ht_tmp);
  FILE *f = fopen(path, "wb");
  fwrite(img, 1, sizeof img, f);
  fclose(f);
  HT_EQ_U64(json_ok(path), 0);
  /* only some sectors written (a torn report): still valid JSON */
  static uint8_t mix[4 * 512];
  xts_spack_t q;
  xts_spack_init(&q, mix, 512, 4, magic);
  memcpy(mix + 512, img + 512, 512);
  f = fopen(path, "wb");
  fwrite(mix, 1, sizeof mix, f);
  fclose(f);
  HT_EQ_U64(json_ok(path), 0);
  static char sq[4096];
  xts_json_squeeze((const char *)img, sizeof img, sq, sizeof sq);
  HT_CHECK(!strncmp(sq, "{\"k0\":\"", 7) && !strstr(sq, "slot_") && !strstr(sq, " ") && sq[strlen(sq) - 1] == '}');
  HT_EQ_U64(ht_count_substr(sq, "\"k"), 9);
  char v[128];
  HT_EQ_U64(xts_json_get(sq, "k8", v, sizeof v), 0);
  HT_EQ_U64(strlen(v), 90);
  HT_CHECK(xts_json_get(sq, "k9", v, sizeof v) != 0);
  xts_json_squeeze((const char *)mix, sizeof mix, sq, sizeof sq);
  HT_CHECK(!strstr(sq, "slot_") && sq[0] == '{' && sq[strlen(sq) - 1] == '}' && sq[1] != ',');
}

HTEST(crashrep_thread_slots)
{
  memset(xts_thrcrash, 0, sizeof xts_thrcrash);
  uint64_t fake[40] = { 0 };
  fake[36] = 0x80004444;
  fake[39] = 2;
  HT_EQ_U64(xts_crash_handle(0x300, fake, 0), XTS_CRASH_THREAD);
  fake[36] = 0x80005555;
  HT_EQ_U64(xts_crash_handle(0x700, fake, 0), XTS_CRASH_THREAD);   /* second crash on PIR 2: counted, the first kept */
  HT_EQ_U64(xts_thrcrash[1].state, 1);
  HT_EQ_U64(xts_thrcrash[1].count, 2);
  HT_EQ_U64(xts_thrcrash[1].srr0, 0x80004444);
  HT_EQ_U64(xts_crash_threads_pending(), 1);
  memset(xts_thrcrash, 0, sizeof xts_thrcrash);
}

/* ---- a family that writes files from inside its cases ---- */
static char gv_path[600];
static int gv_results[3];
static int gv_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 4; i++) xts_case_addf(cl, "g%d", i);
  return 0;
}
static int count_cb(const char *l, void *ud) { (void)l; (void)ud; return 0; }
static int gv_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (ctx->case_index == 1) {
    gv_results[0] = xts_write_small(gv_path, "must not be written\n");
    gv_results[1] = xts_results_foreach(NULL, count_cb, NULL);
    gv_results[2] = xts_mkdirs(gv_path);
    char d[96];
    snprintf(d, sizeof d, "{\"write\":%d,\"foreach\":%d,\"mkdir\":%d}", gv_results[0], gv_results[1], gv_results[2]);
    xts_emit_status(ctx, XTS_ST_INFO, d);
    return 0;
  }
  xts_emit_value(ctx, "count", ctx->case_index, "count", XTS_ST_PASS);
  (void)c;
  return 0;
}
XTS_FAMILY(t_crguard, .kind = "test", .enumerate = gv_enum, .run = gv_run, .block_cases = 4);
static const xts_profile_row_t gv_rows[] = { { .families = "t.crguard" } };
static const xts_profile_t gv_prof = { "tcrguard", "write guard test", gv_rows, 1 };

static int boot_dir(const char *root, const xts_profile_t *p)
{
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    char *argv[] = { "x", "--root", (char *)root, "--quiet", NULL };
    xts_plat_init(4, argv);
    _exit(xts_run(p, NULL) == 0 ? 0 : 1);
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

HTEST(crashrep_write_guard_dir)
{
  char root[512], cmd[700];
  snprintf(root, sizeof root, "%s/crguard", ht_tmp);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  HT_EQ_U64(system(cmd), 0);
  snprintf(gv_path, sizeof gv_path, "%s/xts/host/0001/inside_case.txt", root);
  HT_EQ_U64(boot_dir(root, &gv_prof), 0);
  HT_CHECK(access(gv_path, F_OK) != 0);                /* nothing was written */
  const char *r = rd(root, "results.jsonl");
  HT_CHECK(r && strstr(r, "\"detail\":{\"write\":-1,\"foreach\":-1,\"mkdir\":-1}"));
  HT_EQ_U64(ht_count_substr(r, "\"status\":\"pass\""), 3);
  const char *log = rd(root, "run.log");
  HT_CHECK(log && ht_count_substr(log, "BUG: file ") == 3 && strstr(log, "while cases run refused"));
  const char *sum = rd(root, "summary.json");
  HT_CHECK(sum && strstr(sum, "\"guard_violations\":3"));
  const char *j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "DONE"));
}

/* ---- a block whose records outgrow the sink buffer ---- */
static int big_die = -1;
static int big_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 4; i++) xts_case_addf(cl, "b%d", i);
  return 0;
}
static int big_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (ctx->case_index == big_die) _exit(9);
  static char d[20100];
  int n = snprintf(d, sizeof d, "{\"pad\":\"");
  for (int i = 0; i < 20000; i++) d[n++] = (char)('a' + (i + ctx->case_index) % 26);
  snprintf(d + n, sizeof d - (size_t)n, "\"}");
  for (int k = 0; k < 45; k++) {                      /* ~0.9 MB of results.jsonl per case */
    xts_record_t r;
    xts_record_init(&r);
    r.metric = "count";
    r.unit = "count";
    r.value = k;
    r.detail = d;
    xts_emit(ctx, &r);
  }
  (void)c;
  return 0;
}
XTS_FAMILY(t_crbig, .kind = "test", .enumerate = big_enum, .run = big_run, .block_cases = 4);
static const xts_profile_row_t big_rows[] = { { .families = "t.crbig" } };
static const xts_profile_t big_prof = { "tcrbig", "chunk flush test", big_rows, 1 };

HTEST(crashrep_chunk_flush)
{
  char root[512], cmd[700];
  snprintf(root, sizeof root, "%s/crbig", ht_tmp);
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  HT_EQ_U64(system(cmd), 0);
  big_die = 3;                                         /* the 4th case hangs, after a chunk was written */
  HT_EQ_U64(boot_dir(root, &big_prof), 9);
  const char *j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "PART r0:t.crbig:-:c0 k=2 seq=90\n") && !strstr(j, "END "));
  big_die = -1;
  HT_EQ_U64(boot_dir(root, &big_prof), 0);
  j = rd(root, "journal.txt");
  HT_CHECK(j && strstr(j, "QUAR r0:t.crbig:-:c0 case=3 status=hang") && strstr(j, "DONE"));
  const char *csv = rd(root, "results.csv");
  HT_EQ_U64(ht_count_substr(csv, ",t.crbig,b0,"), 45);   /* written once, before the hang, never re-run */
  HT_EQ_U64(ht_count_substr(csv, ",t.crbig,b1,"), 45);
  HT_EQ_U64(ht_count_substr(csv, ",t.crbig,b2,"), 45);   /* lost with the hang, re-run in boot 2 */
  HT_EQ_U64(ht_count_substr(csv, ",t.crbig,b3,"), 1);    /* the hang record */
  const char *log = rd(root, "run.log");
  HT_CHECK(log && strstr(log, "chunk written at case boundary 2 (PART)") && strstr(log, "starting at case 2"));
  char path[700];
  snprintf(cmd, sizeof cmd, "%s -I tools/check_records.py '%s/xts/host/0001' > /dev/null", py(), root);
  HT_EQ_U64(system(cmd), 0);
  snprintf(path, sizeof path, "%s/xts/host/0001/crash.json", root);
  HT_EQ_U64(json_ok(path), 0);
  snprintf(path, sizeof path, "%s/xts/host/0001/progress.bin", root);
  HT_EQ_U64(json_ok(path), 0);
}
