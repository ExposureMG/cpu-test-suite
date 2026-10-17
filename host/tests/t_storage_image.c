/* The console's storage stack on FAT32 images (host image mode: core -> stdio -> libfat -> disc guard -> image file):
 * clean runs keep the volume consistent (FAT copies mirrored, FSInfo right, pre-existing files untouched), storage
 * errors stop the run with "STORAGE ERROR" and never damage pre-existing content, and a FAT read error during an
 * allocation no longer hands out an in-use cluster (libfat patch e). A small version of `make host-fatreplay`. */
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

static int st_enum(xts_caselist_t *cl)
{
  for (int i = 0; i < 12; i++) xts_case_addf(cl, "c%d", i);
  return 0;
}
static int st_run(const xts_case_t *c, xts_ctx_t *ctx)
{
  char det[1400];
  int n = snprintf(det, sizeof det, "{\"pad\":\"");
  for (int i = 0; i < 1200; i++) det[n++] = (char)('a' + (i * 7 + ctx->case_index) % 26);
  snprintf(det + n, sizeof det - (size_t)n, "\"}");
  xts_record_t r;
  xts_record_init(&r);
  r.metric = "index";
  r.value = ctx->case_index;
  r.detail = det;
  xts_emit(ctx, &r);
  (void)c;
  return 0;
}
XTS_FAMILY(t_storio, .kind = "test", .enumerate = st_enum, .run = st_run, .block_cases = 4);
static const xts_profile_row_t st_rows[] = { { .families = "t.storio" } };
static const xts_profile_t st_prof = { "tstorio", "storage test", st_rows, 1 };

static const char *py(void) { return getenv("PYTHON") ? getenv("PYTHON") : "python3"; }

/* image + manifest under ht_tmp; 0 ok */
static int mkimg(char *img, char *man, size_t n, const char *name, const char *args)
{
  snprintf(img, n, "%s/%s.img", ht_tmp, name);
  snprintf(man, n, "%s/%s.json", ht_tmp, name);
  char cmd[1600];
  snprintf(cmd, sizeof cmd, "%s -I tools/fat_mkimage.py '%s' --populate stick --seed 3 --manifest '%s' %s > /dev/null", py(),
           img, man, args);
  return system(cmd) == 0 ? 0 : -1;
}

/* fat_check one-line summary: returns ok (1/0), -1 if it did not run; *pre / *warn: pre-existing damage, warnings */
static int fatcheck(const char *img, const char *man, int *pre, int *warn, int *struct_err)
{
  char cmd[1600], line[600] = "";
  snprintf(cmd, sizeof cmd, "%s -I tools/fat_check.py '%s' --manifest '%s' --oneline", py(), img, man);
  FILE *p = popen(cmd, "r");
  if (!p) return -1;
  while (fgets(line, sizeof line, p) && strncmp(line, "FATCHECK", 8)) ;
  pclose(p);
  if (strncmp(line, "FATCHECK", 8)) return -1;
  const char *a = strstr(line, " preexisting="), *f = strstr(line, " foreign_page="), *w = strstr(line, " warnings="),
             *s = strstr(line, " struct_errors=");
  if (pre) *pre = (a ? atoi(a + 13) : 99) + (f ? atoi(f + 14) : 99);
  if (warn) *warn = w ? atoi(w + 10) : 99;
  if (struct_err) *struct_err = s ? atoi(s + 15) : 99;
  return strstr(line, "ok=1") != NULL;
}

/* one forked "boot" in image mode; returns the exit status (0 done, 3 stopped: storage poisoned, 1 halted) */
static int boot(const char *img, const char *faults, int guard, const char *errfile)
{
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    int fd = open(errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 2); close(fd); }
    setenv("XTS_FAULTS", faults, 1);
    setenv("XTS_DISC_GUARD", guard ? "1" : "0", 1);
    alarm(60);
    char *argv[] = { "x", "--image", (char *)img, "--quiet", NULL };
    if (xts_plat_init(4, argv) < 0) _exit(5);
    int r = xts_run(&st_prof, NULL);
    _exit(r == -3 ? 3 : r == 0 ? 0 : 4);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* the XTS results.csv of run 0001, read back through libfat (read-only mount) */
static int count_pass(const char *img)
{
  int n = -1;
  if (xts_image_disc_open(img, NULL, 1) < 0 || xts_disc_guard_init(xts_image_disc(), 512, 1) < 0) return -1;
  if (fatMount("tst", xts_disc_guard_iface(), 0, 4, 64)) {
    FILE *f = fopen("tst:/xts/host/0001/results.csv", "r");
    if (f) {
      char line[4096];
      n = 0;
      while (fgets(line, sizeof line, f)) n += strstr(line, ",pass\n") != NULL;
      fclose(f);
    }
    fatUnmount("tst");
  }
  xts_image_disc_close();
  return n;
}

static void clean_run(const char *name, const char *args)
{
  char img[600], man[600], err[700];
  int pre = -1, warn = -1, se = -1;
  HT_EQ_U64(mkimg(img, man, sizeof img, name, args), 0);
  HT_EQ_U64(fatcheck(img, man, &pre, &warn, &se), 1);
  snprintf(err, sizeof err, "%s/%s.err", ht_tmp, name);
  HT_EQ_U64(boot(img, "none", 1, err), 0);
  HT_EQ_U64(fatcheck(img, man, &pre, &warn, &se), 1);  /* FAT copies equal, chains, sizes, no lost clusters */
  HT_EQ_U64(pre, 0);
  HT_EQ_U64(warn, 0);                                   /* FSInfo free count right (written on fsync now) */
  HT_EQ_U64(count_pass(img), 12);
  /* a second boot of the same profile on the same stick: a new run next to the old one, still consistent */
  HT_EQ_U64(boot(img, "none", 1, err), 0);
  HT_EQ_U64(fatcheck(img, man, &pre, &warn, &se), 1);
  HT_EQ_U64(warn, 0);
}

HTEST(storage_image_superfloppy) { clean_run("stor_sf", ""); }
HTEST(storage_image_mbr) { clean_run("stor_mbr", "--layout mbr --size 300M --cluster 4096 --align 8"); }

/* storage errors with the guard: the run stops, says so, and pre-existing content is untouched */
HTEST(storage_poisoned_stops_run)
{
  static const char *const faults[] = { "read_fail=1,after=40", "torn_write=1,after=40", "short_read=1,after=40",
                                        "cbw_read=1,after=40" };
  for (int i = 0; i < 4; i++) {
    char img[600], man[600], err[700], name[32];
    int pre = -1;
    snprintf(name, sizeof name, "stor_poison%d", i);
    HT_EQ_U64(mkimg(img, man, sizeof img, name, ""), 0);
    snprintf(err, sizeof err, "%s/%s.err", ht_tmp, name);
    int rc = boot(img, faults[i], 1, err);
    HT_CHECK(rc == 3 || rc == 1);                       /* stopped (or halted at the start-up write probe) */
    const char *e = ht_slurp(err);
    HT_CHECK(e && strstr(e, "STORAGE ERROR") && strstr(e, "Stop using this stick"));
    fatcheck(img, man, &pre, NULL, NULL);
    HT_EQ_U64(pre, 0);
  }
}

/* libfat patch (e): a FAT read that fails while "xts" is being created (device call 4: the first FAT page) used to make
 * libfat take the root directory's cluster as free, allocate it to the new directory and zero it. Without the guard,
 * the patched libfat must fail the allocation instead. */
HTEST(storage_libfat_fat_read_error)
{
  char img[600], man[600], err[700];
  int pre = -1;
  HT_EQ_U64(mkimg(img, man, sizeof img, "stor_fatrd", ""), 0);
  snprintf(err, sizeof err, "%s/stor_fatrd.err", ht_tmp);
  boot(img, "read_fail=1,at=4,seed=1", 0, err);
  HT_CHECK(fatcheck(img, man, &pre, NULL, NULL) >= 0);
  HT_EQ_U64(pre, 0);
}

/* mini replay: one fault at a random device call, with the guard, both fault models; never pre-existing damage, and a
 * run that finishes leaves a consistent volume */
HTEST(storage_mini_replay)
{
  static const char *const models[] = { "unpatched:1", "patched:1" };
  char img[600], man[600], err[700], base[600], bman[600];
  HT_EQ_U64(mkimg(base, bman, sizeof base, "stor_replay_base", ""), 0);
  int damaged = 0, inconsistent = 0, runs = 0;
  for (int m = 0; m < 2; m++)
    for (int k = 0; k < 6; k++) {
      char faults[96], cmd[1400];
      snprintf(faults, sizeof faults, "%s,seed=%d,at=%d", models[m], k + 1, 5 + 37 * k);
      snprintf(img, sizeof img, "%s/stor_replay.img", ht_tmp);
      snprintf(cmd, sizeof cmd, "cp --sparse=always '%s' '%s'", base, img);
      HT_EQ_U64(system(cmd), 0);
      snprintf(man, sizeof man, "%s", bman);
      snprintf(err, sizeof err, "%s/stor_replay.err", ht_tmp);
      int rc = boot(img, faults, 1, err), pre = -1, se = -1;
      fatcheck(img, man, &pre, NULL, &se);
      runs++;
      damaged += pre != 0;
      inconsistent += rc == 0 && se != 0;
    }
  HT_EQ_U64(runs, 12);
  HT_EQ_U64(damaged, 0);
  HT_EQ_U64(inconsistent, 0);
}
