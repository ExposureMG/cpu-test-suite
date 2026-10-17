/* Crash reports on FAT32 images (host image mode = the console's storage stack: core -> stdio -> libfat -> disc guard ->
 * image), with forked "boots" on images made by tools/fat_mkimage.py (pre-populated like a real stick):
 *   - a crash inside a case: crash.json is filled through the raw API, is valid JSON, the volume is clean
 *     (tools/fat_check.py) and every pre-existing file intact; the next boot turns it into a crash record, quarantines
 *     exactly that case, re-arms the files, finishes and unmounts cleanly;
 *   - a hang (the boot is killed inside a case): progress.bin names the case, the volume is clean; the next boot
 *     quarantines it as a hang;
 *   - a crash while the storage stack is busy (inside a device write of the disc guard): the crash path does not touch
 *     the stick (no device write after the crash), crash.json stays armed, the volume is clean;
 *   - a file write attempted while cases run: refused, the volume is clean, the file does not exist. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fat.h>
#include <xts/xts.h>
#include <core/core.h>
#include <plat/disc_guard.h>
#include <plat/disc_host.h>
#include "htest.h"

static int ci_crash = -1, ci_hang = -1, ci_write = -1, ci_busy_at = -1;
static int ci_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 12; i++) xts_case_addf(cl, "c%d", i);
  return 0;
}
static int ci_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  if (ctx->case_index == ci_crash) xts_plat_self_crash();          /* crash.json, _exit(9) */
  if (ctx->case_index == ci_hang) _exit(9);                         /* hang / power cut */
  if (ctx->case_index == ci_write) {
    char p[64];
    snprintf(p, sizeof p, "%sxts/inside.txt", xts_plat_root());
    int r = xts_write_small(p, "no\n");
    xts_emit_status(ctx, r < 0 ? XTS_ST_PASS : XTS_ST_FAIL, r < 0 ? "{\"guard\":\"refused\"}" : "{\"guard\":\"WRITTEN\"}");
    return 0;
  }
  xts_emit_value(ctx, "count", ctx->case_index, "count", XTS_ST_PASS);
  (void)c;
  return 0;
}
XTS_FAMILY(t_crimg, .kind = "test", .enumerate = ci_enum, .run = ci_run, .block_cases = 4);
static const xts_profile_row_t ci_rows[] = { { .families = "t.crimg" } };
static const xts_profile_t ci_prof = { "tcrimg", "crash report image test", ci_rows, 1 };

static const char *py(void) { return getenv("PYTHON") ? getenv("PYTHON") : "python3"; }

static int mkimg(char *img, char *man, size_t n, const char *name)
{
  snprintf(img, n, "%s/%s.img", ht_tmp, name);
  snprintf(man, n, "%s/%s.json", ht_tmp, name);
  char cmd[1600];
  snprintf(cmd, sizeof cmd, "%s -I tools/fat_mkimage.py '%s' --populate stick --seed 5 --manifest '%s' > /dev/null", py(),
           img, man);
  return system(cmd) == 0 ? 0 : -1;
}

/* tools/fat_check.py: 1 = ok with no pre-existing damage and no warnings */
static int fat_clean(const char *img, const char *man)
{
  char cmd[1600], line[600] = "";
  snprintf(cmd, sizeof cmd, "%s -I tools/fat_check.py '%s' --manifest '%s' --oneline", py(), img, man);
  FILE *p = popen(cmd, "r");
  if (!p) return -1;
  while (fgets(line, sizeof line, p) && strncmp(line, "FATCHECK", 8)) ;
  pclose(p);
  if (!strstr(line, "ok=1")) { printf("    fat_check: %s", line); return 0; }
  const char *a = strstr(line, " preexisting="), *w = strstr(line, " warnings="), *s = strstr(line, " struct_errors=");
  int ok = a && atoi(a + 13) == 0 && w && atoi(w + 10) == 0 && s && atoi(s + 15) == 0;
  if (!ok) printf("    fat_check: %s", line);
  return ok;
}

/* busy hook: on the n-th device write while cases run (a breadcrumb write, inside the disc guard), crash */
static int busy_writes;
static void busy_hook(int write, uint32_t sector, uint32_t count)
{
  (void)sector;
  (void)count;
  if (!write || !xts_cases_running || ++busy_writes != ci_busy_at) return;
  xts_fault_stats_t before, after;
  xts_image_fault_stats(&before);
  uint64_t ctx[40] = { 0 };
  ctx[36] = 0x80012345;
  ctx[38] = 0;
  int o = xts_crash_handle(0x300, ctx, 0x42000000u);
  xts_image_fault_stats(&after);
  xts_host_crash_screen();
  _exit(o == XTS_CRASH_NOT_SAVED_BUSY && after.writes == before.writes && after.reads == before.reads ? 21 : 22);
}

static int boot(const char *img, const char *errfile, int crash, int hang, int wr, int busy_at)
{
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    int fd = open(errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 2); close(fd); }
    setenv("XTS_FAULTS", "none", 1);
    setenv("XTS_DISC_GUARD", "1", 1);
    alarm(120);
    ci_crash = crash;
    ci_hang = hang;
    ci_write = wr;
    ci_busy_at = busy_at;
    xts_host_crash_sim = crash >= 0;
    if (busy_at > 0) xts_host_disc_hook = busy_hook;
    char *argv[] = { "x", "--image", (char *)img, "--quiet", NULL };
    if (xts_plat_init(4, argv) < 0) _exit(5);
    int r = xts_run(&ci_prof, NULL);
    _exit(r == -3 ? 3 : r == 0 ? 0 : 4);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* copy files of run 0001 out of the image (read-only mount) into <dir>/; returns how many were found */
static int extract(const char *img, const char *dir)
{
  static const char *const files[] = { "crash.json", "progress.bin", "journal.txt", "results.jsonl", "results.csv",
                                       "failures.jsonl", "run.log", "summary.json", "crash_boot1.json", "sysinfo.json" };
  char cmd[700];
  snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
  if (system(cmd) != 0) return -1;
  int n = 0;
  if (xts_image_disc_open(img, NULL, 1) < 0 || xts_disc_guard_init(xts_image_disc(), 512, 1) < 0) return -1;
  if (fatMount("tst", xts_disc_guard_iface(), 0, 4, 64)) {
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
      char src[200], dst[700];
      snprintf(src, sizeof src, "tst:/xts/host/0001/%s", files[i]);
      FILE *f = fopen(src, "rb");
      if (!f) continue;
      snprintf(dst, sizeof dst, "%s/%s", dir, files[i]);
      FILE *o = fopen(dst, "wb");
      static char buf[1 << 16];
      size_t k;
      while ((k = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, k, o);
      fclose(o);
      fclose(f);
      n++;
    }
    fatUnmount("tst");
  }
  xts_image_disc_close();
  return n;
}

static const char *rd(const char *dir, const char *f)
{
  char p[700];
  snprintf(p, sizeof p, "%s/%s", dir, f);
  return ht_slurp(p);
}

static int json_state(const char *dir, const char *f, const char *state)
{
  char cmd[1200];
  snprintf(cmd, sizeof cmd, "%s -I -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d.get(\"state\")==sys.argv[2] else 3)' "
           "'%s/%s' '%s' 2>/dev/null", py(), dir, f, state);
  return system(cmd) == 0;
}

HTEST(crashrep_image_crash_in_case)
{
  char img[600], man[600], err[700], dir[700], cmd[900];
  HT_EQ_U64(mkimg(img, man, sizeof img, "crimg_crash"), 0);
  snprintf(err, sizeof err, "%s/crimg_crash.err", ht_tmp);
  snprintf(dir, sizeof dir, "%s/crimg_crash.out", ht_tmp);
  HT_EQ_U64(boot(img, err, 6, -1, -1, 0), 9);           /* case 6 = block c1, k=2 */
  const char *e = ht_slurp(err);
  HT_CHECK(e && strstr(e, "report saved to img:/xts/host/0001/crash.json") && strstr(e, "stick is safe to reset"));
  HT_EQ_U64(fat_clean(img, man), 1);                     /* no unmount, no flush after the crash: still consistent */
  HT_CHECK(extract(img, dir) >= 5);
  HT_CHECK(json_state(dir, "crash.json", "crashed"));    /* valid JSON as written raw by the crash path */
  HT_CHECK(json_state(dir, "progress.bin", "case"));
  const char *cj = rd(dir, "crash.json");
  HT_CHECK(cj && strstr(cj, "\"block\":\"r0:t.crimg:-:c1\"") && strstr(cj, "\"case_rel\":2") && strstr(cj, "\"case_id\":\"c6\""));
  HT_CHECK(cj && strstr(cj, "\"storage_busy\":0") && strstr(cj, "\"fat_busy\":0") && strstr(cj, "\"cases_running\":1"));
  const char *j = rd(dir, "journal.txt");
  HT_CHECK(j && strstr(j, "BEGIN r0:t.crimg:-:c1\n") && !strstr(j, "END r0:t.crimg:-:c1"));
  /* boot 2: ingest, quarantine, re-arm, finish, unmount */
  HT_EQ_U64(boot(img, err, -1, -1, -1, 0), 0);
  HT_EQ_U64(fat_clean(img, man), 1);
  HT_CHECK(extract(img, dir) >= 8);
  j = rd(dir, "journal.txt");
  HT_CHECK(j && strstr(j, "QUAR r0:t.crimg:-:c1 case=2 status=crash") && strstr(j, "REPORT tb=") && strstr(j, "DONE"));
  const char *fl = rd(dir, "failures.jsonl");
  HT_CHECK(fl && ht_count_substr(fl, "\n") == 1 && strstr(fl, "\"case_id\":\"c6\"") && strstr(fl, "\"status\":\"crash\"") &&
           strstr(fl, "\"metric\":\"result\"") && strstr(fl, "\"crash_report\":{\"state\":\"crashed\""));
  const char *csv = rd(dir, "results.csv");
  HT_EQ_U64(ht_count_substr(csv, ",pass\n"), 11);
  HT_CHECK(json_state(dir, "crash.json", "armed"));      /* re-armed for boot 2 */
  HT_CHECK(rd(dir, "crash_boot1.json") && json_state(dir, "crash_boot1.json", "crashed"));
  const char *log = rd(dir, "run.log");
  HT_CHECK(log && strstr(log, "previous boot: crash.json filled") && strstr(log, "run end: summary.json written"));
  snprintf(cmd, sizeof cmd, "%s -I tools/check_records.py '%s' > /dev/null", py(), dir);
  HT_EQ_U64(system(cmd), 0);
}

HTEST(crashrep_image_hang)
{
  char img[600], man[600], err[700], dir[700];
  HT_EQ_U64(mkimg(img, man, sizeof img, "crimg_hang"), 0);
  snprintf(err, sizeof err, "%s/crimg_hang.err", ht_tmp);
  snprintf(dir, sizeof dir, "%s/crimg_hang.out", ht_tmp);
  HT_EQ_U64(boot(img, err, -1, 9, -1, 0), 9);           /* case 9 = block c2, k=1 */
  HT_EQ_U64(fat_clean(img, man), 1);
  HT_CHECK(extract(img, dir) >= 5);
  HT_CHECK(json_state(dir, "progress.bin", "case") && json_state(dir, "crash.json", "armed"));
  const char *pb = rd(dir, "progress.bin");
  HT_CHECK(pb && strstr(pb, "\"block\":\"r0:t.crimg:-:c2\"") && strstr(pb, "\"case_id\":\"c9\"") && strstr(pb, "\"every\":1"));
  HT_EQ_U64(boot(img, err, -1, -1, -1, 0), 0);
  HT_EQ_U64(fat_clean(img, man), 1);
  HT_CHECK(extract(img, dir) >= 8);
  const char *j = rd(dir, "journal.txt");
  HT_CHECK(j && strstr(j, "QUAR r0:t.crimg:-:c2 case=1 status=hang") && strstr(j, "DONE"));
  const char *fl = rd(dir, "failures.jsonl");
  HT_CHECK(fl && strstr(fl, "\"case_id\":\"c9\"") && strstr(fl, "\"status\":\"hang\"") && strstr(fl, "\"breadcrumb\":{\"state\":\"case\""));
}

HTEST(crashrep_image_crash_while_busy)
{
  char img[600], man[600], err[700], dir[700];
  HT_EQ_U64(mkimg(img, man, sizeof img, "crimg_busy"), 0);
  snprintf(err, sizeof err, "%s/crimg_busy.err", ht_tmp);
  snprintf(dir, sizeof dir, "%s/crimg_busy.out", ht_tmp);
  /* the 7th device write while cases run: inside the disc guard (a breadcrumb write: read-back is not a write, so each
   * breadcrumb is one device write) */
  HT_EQ_U64(boot(img, err, -1, -1, -1, 7), 21);          /* 21: outcome not_saved_storage_busy and no device I/O */
  const char *e = ht_slurp(err);
  HT_CHECK(e && strstr(e, "report NOT saved: crash inside the USB driver") && strstr(e, "progress.bin names the case"));
  HT_EQ_U64(fat_clean(img, man), 1);
  HT_CHECK(extract(img, dir) >= 5);
  HT_CHECK(json_state(dir, "crash.json", "armed"));       /* untouched */
  HT_EQ_U64(boot(img, err, -1, -1, -1, 0), 0);
  HT_EQ_U64(fat_clean(img, man), 1);
  HT_CHECK(extract(img, dir) >= 8);
  const char *j = rd(dir, "journal.txt");
  HT_CHECK(j && strstr(j, "DONE") && strstr(j, "status=hang"));
}

HTEST(crashrep_image_write_guard)
{
  char img[600], man[600], err[700], dir[700];
  HT_EQ_U64(mkimg(img, man, sizeof img, "crimg_guard"), 0);
  snprintf(err, sizeof err, "%s/crimg_guard.err", ht_tmp);
  snprintf(dir, sizeof dir, "%s/crimg_guard.out", ht_tmp);
  HT_EQ_U64(boot(img, err, -1, -1, 5, 0), 0);
  HT_EQ_U64(fat_clean(img, man), 1);
  HT_CHECK(extract(img, dir) >= 8);
  const char *r = rd(dir, "results.jsonl");
  HT_CHECK(r && strstr(r, "\"guard\":\"refused\"") && !strstr(r, "WRITTEN"));
  const char *sum = rd(dir, "summary.json");
  HT_CHECK(sum && strstr(sum, "\"guard_violations\":1"));
  /* the file is not on the image */
  int found = -1;
  if (xts_image_disc_open(img, NULL, 1) == 0 && xts_disc_guard_init(xts_image_disc(), 512, 1) == 0 &&
      fatMount("tst", xts_disc_guard_iface(), 0, 4, 64)) {
    FILE *f = fopen("tst:/xts/inside.txt", "rb");
    found = f != NULL;
    if (f) fclose(f);
    fatUnmount("tst");
  }
  xts_image_disc_close();
  HT_EQ_U64(found, 0);
}
