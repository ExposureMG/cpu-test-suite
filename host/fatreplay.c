/* host-fatreplay: replay the console's storage stack on FAT32 images with injected USB faults.
 *
 *   make host-fatreplay [PROFILE=smoke] [FATREPLAY_ARGS="--runs 40 --layout both"]
 *
 * Every run copies a pre-populated FAT32 image (tools/fat_mkimage.py --populate stick: ~90 files and directories,
 * 32 KB clusters, superfloppy like the user's stick, or MBR), then runs the profile's dry run in a child process with
 * the console's real storage path: core sinks/journal -> stdio -> libfat -> [disc guard] -> image disc with injected
 * faults (src/plat/disc_image_host.c). Afterwards tools/fat_check.py checks the image (structure, FAT copies, lost
 * chains, entries pointing to free clusters, every pre-existing entry byte-identical: --manifest; forensics for stale
 * pages written over directories / the FAT and USB protocol blocks in sectors) and the XTS output files are compared
 * with a fault-free reference run. Before the scenarios, three fault-free runs in a row on one image are checked for
 * both libfat builds.
 *
 * Scenarios (libfat build, disc guard, fault model; "1" = one fault at a random device call, "x%" = every device call
 * of that kind faulty with that probability):
 *   A1-A3  upstream libfat, no guard, unpatched-driver faults (short/stale reads, CSW in data, torn writes, CBW in data)
 *   B1-B2  upstream libfat, no guard, patched-driver faults (calls that fail after a partial transfer)
 *   C2     patched libfat, no guard, patched-driver faults
 *   D1-D3  patched libfat + guard, unpatched-driver faults
 *   E1-E2  patched libfat + guard, patched-driver faults (= XTS with USB_FIX=1)
 *   F1     patched libfat + guard, desync faults (A's + misdirected writes: data written at the previous command's LBA,
 *          which no check can see before it happened; only the USB driver fix removes that class)
 * Verdicts: A/B "incident class reproduced" = a pre-existing entry, chain or content changed or a stale page was written
 * over pre-existing metadata; D/E "safe" = never that, and never a run that finished without a reported error but left
 * a damaged volume or wrong XTS files.
 *
 * The same source is linked twice (Makefile): xts-fatreplay-<p> (patched libfat: the parent and the patched children)
 * and xts-fatreplay-upstream-<p> (upstream libfat: children of A and B, via --upstream).
 * Options: --runs N (per scenario, default 30), --seed S, --layout superfloppy|mbr|both, --scenarios A1,D2,...,
 * --work DIR, --python PY, --tools DIR, --keep N (images of notable runs kept per scenario, default 2), --verbose.
 * Child mode (used internally): --child IMAGE FAULTSPEC GUARD STATSFILE.
 * Exit status 1 if a patched-stack scenario (D*, E*) damaged pre-existing content or finished silently corrupt. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fat.h>
#include <xts/xts.h>
#include <core/core.h>
#include <plat/disc_guard.h>
#include <plat/disc_host.h>

#define CHILD_TIMEOUT_S 120

/* ------------------------------------------------------------------------------------------------ child (one run) */
static const char *stats_path;

static void write_stats(void)
{
  if (!stats_path) return;
  FILE *f = fopen(stats_path, "w");
  if (!f) return;
  xts_fault_stats_t fs;
  xts_storage_status_t st;
  xts_image_fault_stats(&fs);
  xts_plat_storage_status(&st);
  fprintf(f, "calls=%llu reads=%llu writes=%llu injected=%llu short_read=%llu cbw_read=%llu torn_write=%llu cbw_write=%llu "
          "misdirect=%llu read_fail=%llu write_fail=%llu poisoned=%d read_retries=%llu repaired=%llu io_errors=%u guard_ms=%.1f "
          "op=%s why=%s\n",
          (unsigned long long)fs.calls, (unsigned long long)fs.reads, (unsigned long long)fs.writes,
          (unsigned long long)fs.injected, (unsigned long long)fs.n_short_read, (unsigned long long)fs.n_cbw_read,
          (unsigned long long)fs.n_torn_write, (unsigned long long)fs.n_cbw_write, (unsigned long long)fs.n_misdirect,
          (unsigned long long)fs.n_read_fail, (unsigned long long)fs.n_write_fail, st.poisoned,
          (unsigned long long)st.read_retries, (unsigned long long)st.write_repaired, xts_io_error_count(), st.guard_ms,
          st.op[0] ? st.op : "-",
          st.why[0] ? st.why : "-");
  fclose(f);
}

static int child_main(const char *image, const char *faults, int guard, const char *stats)
{
  stats_path = stats;
  setenv("XTS_FAULTS", faults, 1);
  setenv("XTS_DISC_GUARD", guard ? "1" : "0", 1);
  setenv("XTS_QUIET", "1", 1);
  alarm(CHILD_TIMEOUT_S);
  atexit(write_stats);                      /* also on xts_plat_halt's exit(1) */
  char *av[] = { "fatreplay", "--image", (char *)image, "--quiet", NULL };
  if (xts_plat_init(4, av) < 0) return 5;
  xts_run_opts_t o;
  memset(&o, 0, sizeof o);
  o.dryrun = 1;
  int r = xts_run(&xts_profile, &o);
  return r == -3 ? 3 : r == 0 ? 0 : 4;
}

/* ------------------------------------------------------------------------------------------------ parent helpers */
static const char *work = "build/fatreplay", *python = "python3", *tools = "tools", *upstream_bin;
static char self_bin[1024];
static int verbose;

static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int sh(const char *fmt, ...)
{
  char cmd[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cmd, sizeof cmd, fmt, ap);
  va_end(ap);
  if (verbose) fprintf(stderr, "+ %s\n", cmd);
  int st = system(cmd);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* sparse copy (SEEK_DATA / SEEK_HOLE) */
static int copy_sparse(const char *src, const char *dst)
{
  int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (in < 0 || out < 0) { if (in >= 0) close(in); if (out >= 0) close(out); return -1; }
  struct stat st;
  fstat(in, &st);
  static char buf[1 << 20];
  off_t pos = 0;
  int rc = 0;
  while (pos < st.st_size) {
    off_t d = lseek(in, pos, SEEK_DATA);
    if (d < 0) break;
    off_t h = lseek(in, d, SEEK_HOLE);
    if (h < 0) h = st.st_size;
    for (off_t o = d; o < h;) {
      size_t n = (size_t)(h - o > (off_t)sizeof buf ? (off_t)sizeof buf : h - o);
      ssize_t r = pread(in, buf, n, o);
      if (r <= 0 || pwrite(out, buf, (size_t)r, o) != r) { rc = -1; break; }
      o += r;
    }
    pos = h;
  }
  if (ftruncate(out, st.st_size) < 0) rc = -1;
  close(in);
  close(out);
  return rc;
}

static long kv(const char *line, const char *key)
{
  char k[64];
  snprintf(k, sizeof k, " %s=", key);
  const char *p = strstr(line, k);
  if (!p && !strncmp(line, k + 1, strlen(k + 1))) p = line - 1;
  return p ? strtol(p + strlen(k), NULL, 10) : -1;
}

/* run one child; returns the wait status */
static int run_child(const char *bin, const char *image, const char *faults, int guard, const char *stats, const char *log)
{
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    char g[4];
    snprintf(g, sizeof g, "%d", guard);
    setenv("ASAN_OPTIONS", "detect_leaks=0:exitcode=86", 1);
    execl(bin, bin, "--child", image, faults, g, stats, (char *)NULL);
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return st;
}

/* ---- XTS output compared with the reference run ---- */
static const char *const cmp_files[] = { "results.jsonl", "results.csv", "journal.txt" };
#define NCMP 3
static char *ref_data[NCMP];
static size_t ref_len[NCMP];

static int read_all(const char *path, char **out, size_t *len)
{
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t cap = 1 << 16, n = 0;
  char *b = malloc(cap);
  for (;;) {
    if (n == cap) b = realloc(b, cap *= 2);
    size_t r = fread(b + n, 1, cap - n, f);
    if (!r) break;
    n += r;
  }
  int err = ferror(f);
  fclose(f);
  *out = b;
  *len = n;
  return err ? -2 : 0;
}

static int mount_ro(const char *image)
{
  if (xts_image_disc_open(image, NULL, 1) < 0) return -1;
  if (xts_disc_guard_init(xts_image_disc(), 512, 1) < 0) return -1;
  return fatMount("chk", xts_disc_guard_iface(), 0, 8, 64) ? 0 : -1;
}

/* in a forked child (a damaged image must not hang or crash the parent): 0 identical, 1 prefix (partial run),
 * 2 corrupt (a file is not a prefix of the reference), 3 no output, 4 cannot mount */
static int xts_output_state(const char *image)
{
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    alarm(30);
    if (mount_ro(image) < 0) _exit(4);
    int worst = 0, any = 0;
    for (int i = 0; i < NCMP; i++) {
      char p[128], *d = NULL;
      size_t n = 0;
      snprintf(p, sizeof p, "chk:/xts/host/0001/%s", cmp_files[i]);
      int r = read_all(p, &d, &n);
      if (r == -1) continue;
      any = 1;
      int st = r < 0 ? 2 : (n == ref_len[i] && !memcmp(d, ref_data[i], n)) ? 0
                         : (n < ref_len[i] && !memcmp(d, ref_data[i], n)) ? 1 : 2;
      if (st > worst) worst = st;
      free(d);
    }
    _exit(any ? worst : 3);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 2;
}

/* ------------------------------------------------------------------------------------------------ scenarios */
typedef struct {
  const char *id;
  const char *name;
  int upstream, guard;
  const char *faults;         /* fault spec (plat/disc_host.h) */
  int single;                 /* 1: exactly one fault at a random device call ("at="), 0: the rates above all run long */
  int patched_stack;          /* must be safe */
  /* results */
  int runs, injected, faults_total, completed, identical, partial, silent_corrupt, poisoned, halted, crashed, hung, mount_failed;
  int fs_errors, preexisting, foreign_page, usb_garbage, fat_mismatch, lost, read_recovered, kept;
  int xts_corrupt, io_reported, fat2_pre, dangling, outside;
} scen_t;

static scen_t scen[] = {
  { "A1", "upstream libfat, no guard: 1 unpatched-driver fault", 1, 0, "unpatched:1", 1, 0 },
  { "A2", "upstream libfat, no guard: short reads, 2% of reads", 1, 0, "short_read=0.02", 0, 0 },
  { "A3", "upstream libfat, no guard: unpatched-driver faults, 1%", 1, 0, "unpatched:0.01", 0, 0 },
  { "B1", "upstream libfat, no guard: 1 patched-driver fault", 1, 0, "patched:1", 1, 0 },
  { "B2", "upstream libfat, no guard: patched-driver faults, 1%", 1, 0, "patched:0.01", 0, 0 },
  { "C2", "patched libfat, no guard: patched-driver faults, 1%", 0, 0, "patched:0.01", 0, 0 },
  { "D1", "patched libfat + guard: 1 unpatched-driver fault", 0, 1, "unpatched:1", 1, 1 },
  { "D2", "patched libfat + guard: short reads, 2% of reads", 0, 1, "short_read=0.02", 0, 1 },
  { "D3", "patched libfat + guard: unpatched-driver faults, 1%", 0, 1, "unpatched:0.01", 0, 1 },
  { "E1", "patched libfat + guard: 1 patched-driver fault (XTS)", 0, 1, "patched:1", 1, 1 },
  { "E2", "patched libfat + guard: patched-driver faults, 1% (XTS)", 0, 1, "patched:0.01", 0, 1 },
  { "F1", "patched libfat + guard: 1 desync fault (+misdirected)", 0, 1, "desync:1", 1, 0 },
};
#define NSCEN (int)(sizeof scen / sizeof scen[0])

static uint64_t rng_state = 1;
static uint32_t rnd(uint32_t n)
{
  rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return (uint32_t)((rng_state >> 33) % n);
}

static int make_base(const char *layout, char *img, char *man, size_t n)
{
  snprintf(img, n, "%s/base-%s.img", work, layout);
  snprintf(man, n, "%s/base-%s.json", work, layout);
  int r = sh("%s -I %s/fat_mkimage.py '%s' --layout %s --populate stick --seed 7 --manifest '%s' > '%s/base-%s.txt'", python,
             tools, img, layout, man, work, layout);
  return r == 0 ? 0 : -1;
}

static int fat_check(const char *img, const char *man, char *line, size_t n)
{
  char cmd[2048];
  snprintf(cmd, sizeof cmd, "%s -I %s/fat_check.py '%s' --manifest '%s' --oneline", python, tools, img, man);
  FILE *p = popen(cmd, "r");
  if (!p) return -1;
  line[0] = 0;
  char buf[512];
  while (fgets(buf, sizeof buf, p))
    if (!strncmp(buf, "FATCHECK", 8)) snprintf(line, n, "%s", buf);
  line[strcspn(line, "\n")] = 0;
  pclose(p);
  return line[0] ? 0 : -1;
}

int main(int argc, char **argv)
{
  if (argc >= 6 && !strcmp(argv[1], "--child")) return child_main(argv[2], argv[3], atoi(argv[4]), argv[5]);

  int runs = 30, keep = 2;
  const char *layout_opt = "superfloppy", *which = "A1,A2,A3,B1,B2,C2,D1,D2,D3,E1,E2,F1";
  uint64_t seed = 1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--layout") && i + 1 < argc) layout_opt = argv[++i];
    else if (!strcmp(argv[i], "--scenarios") && i + 1 < argc) which = argv[++i];
    else if (!strcmp(argv[i], "--work") && i + 1 < argc) work = argv[++i];
    else if (!strcmp(argv[i], "--python") && i + 1 < argc) python = argv[++i];
    else if (!strcmp(argv[i], "--tools") && i + 1 < argc) tools = argv[++i];
    else if (!strcmp(argv[i], "--upstream") && i + 1 < argc) upstream_bin = argv[++i];
    else if (!strcmp(argv[i], "--keep") && i + 1 < argc) keep = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    else { fprintf(stderr, "usage: see host/fatreplay.c\n"); return 2; }
  }
  rng_state = seed * 2654435761u + 1;
  ssize_t sl = readlink("/proc/self/exe", self_bin, sizeof self_bin - 1);
  if (sl <= 0) { fprintf(stderr, "fatreplay: cannot find my own binary\n"); return 2; }
  self_bin[sl] = 0;
  if (strpbrk(which, "AB") && !upstream_bin) { fprintf(stderr, "fatreplay: scenarios A*/B* need --upstream <bin>\n"); return 2; }
  if (sh("mkdir -p '%s/keep'", work)) return 2;

  const char *layouts[2] = { "superfloppy", "mbr" };
  int nlayouts = !strcmp(layout_opt, "both") ? 2 : 1;
  if (!strcmp(layout_opt, "mbr")) layouts[0] = "mbr";
  char base[2][600], man[2][600];
  long ref_calls[2][2] = { { 0, 0 }, { 0, 0 } };      /* [patched=0 / upstream=1][layout] */
  for (int l = 0; l < nlayouts; l++) {
    if (make_base(layouts[l], base[l], man[l], sizeof base[l]) < 0) { fprintf(stderr, "fat_mkimage failed\n"); return 2; }
    /* reference run: patched stack, no faults */
    char img[700], stats[700], log[700], line[512];
    snprintf(img, sizeof img, "%s/ref-%s.img", work, layouts[l]);
    snprintf(stats, sizeof stats, "%s/ref-%s.stats", work, layouts[l]);
    snprintf(log, sizeof log, "%s/ref-%s.log", work, layouts[l]);
    if (copy_sparse(base[l], img) < 0) return 2;
    int st = run_child(self_bin, img, "none", 1, stats, log);
    char *s = NULL;
    size_t sn = 0;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || read_all(stats, &s, &sn) < 0) {
      fprintf(stderr, "fatreplay: the fault-free reference run failed (see %s)\n", log);
      return 2;
    }
    s[sn < 1 ? 0 : sn - 1] = 0;
    snprintf(line, sizeof line, " %s", s);
    ref_calls[0][l] = kv(line, "calls");
    free(s);
    if (upstream_bin) {                       /* upstream libfat makes fewer device calls (FAT #2 never written) */
      char uimg[700];
      snprintf(uimg, sizeof uimg, "%s/ref-upstream-%s.img", work, layouts[l]);
      if (copy_sparse(base[l], uimg) < 0) return 2;
      st = run_child(upstream_bin, uimg, "none", 0, stats, log);
      if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || read_all(stats, &s, &sn) < 0) {
        fprintf(stderr, "fatreplay: the fault-free upstream reference run failed (see %s)\n", log);
        return 2;
      }
      s[sn < 1 ? 0 : sn - 1] = 0;
      snprintf(line, sizeof line, " %s", s);
      ref_calls[1][l] = kv(line, "calls");
      free(s);
    }
    if (fat_check(img, man[l], line, sizeof line) < 0 || kv(line, "ok") != 1) {
      fprintf(stderr, "fatreplay: the reference image does not check clean: %s\n", line);
      return 2;
    }
    if (l == 0) {
      if (mount_ro(img) < 0) { fprintf(stderr, "fatreplay: cannot mount the reference image\n"); return 2; }
      for (int i = 0; i < NCMP; i++) {
        char p[128];
        snprintf(p, sizeof p, "chk:/xts/host/0001/%s", cmp_files[i]);
        if (read_all(p, &ref_data[i], &ref_len[i]) < 0) { fprintf(stderr, "fatreplay: reference %s missing\n", p); return 2; }
      }
      fatUnmount("chk");
      xts_image_disc_close();
    }
    printf("reference (%s): %ld device calls (upstream libfat: %ld), results.jsonl %zu bytes, fat_check clean\n", layouts[l],
           ref_calls[0][l], ref_calls[1][l], ref_len[0]);
  }

  /* fault-free: three consecutive runs on the same stick (a new run each time, like booting the ELF again) */
  for (int u = 0; u < 2; u++) {
    if (u && !upstream_bin) break;
    char img[700], stats[700], log[700], line[512];
    snprintf(img, sizeof img, "%s/three-%s.img", work, u ? "upstream" : "patched");
    snprintf(stats, sizeof stats, "%s/three.stats", work);
    snprintf(log, sizeof log, "%s/three.log", work);
    if (copy_sparse(base[0], img) < 0) return 2;
    int fails = 0;
    for (int k = 0; k < 3; k++) {
      int st = run_child(u ? upstream_bin : self_bin, img, "none", !u, stats, log);
      fails += !WIFEXITED(st) || WEXITSTATUS(st) != 0;
    }
    fat_check(img, man[0], line, sizeof line);
    printf("3 fault-free runs in a row, %s libfat%s: %s%s\n", u ? "upstream" : "patched", u ? ", no guard" : " + guard",
           fails ? "a run FAILED; " : "", line[0] ? line + 9 : "fat_check failed");
    if (!u && kv(line, "ok") != 1) { fprintf(stderr, "fatreplay: the patched stack is not clean without faults\n"); return 2; }
  }

  int bad = 0;
  for (int si = 0; si < NSCEN; si++) {
    scen_t *S = &scen[si];
    if (!strstr(which, S->id)) continue;
    const char *bin = S->upstream ? upstream_bin : self_bin;
    for (int r = 0; r < runs; r++) {
      int l = r % nlayouts;
      char img[700], stats[700], log[700], faults[256], line[512];
      snprintf(img, sizeof img, "%s/run.img", work);
      snprintf(stats, sizeof stats, "%s/run.stats", work);
      snprintf(log, sizeof log, "%s/run.log", work);
      uint32_t at = 1 + rnd((uint32_t)ref_calls[S->upstream][l]);
      if (S->single) snprintf(faults, sizeof faults, "%s,seed=%d,at=%u", S->faults, r + 1, at);
      else snprintf(faults, sizeof faults, "%s,seed=%d", S->faults, r + 1);
      if (copy_sparse(base[l], img) < 0) return 2;
      unlink(stats);
      int st = run_child(bin, img, faults, S->guard, stats, log);
      S->runs++;
      char *s = NULL;
      size_t sn = 0;
      char st_line[1024] = "";
      if (read_all(stats, &s, &sn) == 0) { snprintf(st_line, sizeof st_line, " %.*s", (int)sn, s); free(s); }
      int injected = kv(st_line, "injected") > 0, poisoned = kv(st_line, "poisoned") == 1;
      S->injected += injected;
      S->faults_total += (int)(kv(st_line, "injected") > 0 ? kv(st_line, "injected") : 0);
      if (kv(st_line, "read_retries") > 0) S->read_recovered++;
      const char *outcome;
      if (WIFSIGNALED(st)) {
        if (WTERMSIG(st) == SIGALRM) { S->hung++; outcome = "hang"; }
        else { S->crashed++; outcome = "crash"; }
      } else {
        int ec = WEXITSTATUS(st);
        if (ec == 0) { S->completed++; outcome = "completed"; }
        else if (ec == 3 || poisoned) { S->poisoned++; outcome = "stopped (storage poisoned)"; }
        else if (ec == 5) { S->mount_failed++; outcome = "mount failed"; }
        else if (ec == 86) { S->crashed++; outcome = "crash (ASan)"; }
        else { S->halted++; outcome = "halted"; }
      }
      fat_check(img, man[l], line, sizeof line);
      /* structural errors other than FAT #2 being stale (upstream libfat never writes it: counted separately) */
      int ok = kv(line, "struct_errors") == 0, pre = kv(line, "preexisting") > 0, fp = kv(line, "foreign_page") > 0,
          ug = kv(line, "usb_garbage") > 0, ioerr = kv(st_line, "io_errors") > 0;
      int xs = xts_output_state(img);
      if (!ok) S->fs_errors++;
      if (pre || fp) S->preexisting++;
      if (fp) S->foreign_page++;
      if (ug) S->usb_garbage++;
      if (kv(line, "fat_mismatch") > 0) S->fat_mismatch++;
      if (kv(line, "preexisting_fat2") > 0) S->fat2_pre++;
      if (kv(line, "dangling") > 0) S->dangling++;
      if (kv(line, "outside_xts") > 0) S->outside++;
      if (kv(line, "lost") > 0) S->lost++;
      if (xs == 0) S->identical++;
      else if (xs == 1) S->partial++;
      else if (xs == 2) S->xts_corrupt++;
      /* finished as if nothing happened, but the stick or the output is wrong */
      int silent = !strcmp(outcome, "completed") && !ioerr && (!ok || xs == 2);
      if (ioerr && !strcmp(outcome, "completed")) S->io_reported++;
      if (silent) S->silent_corrupt++;
      int notable = pre || fp || silent || (S->patched_stack && !ok);
      if (verbose || (notable && S->kept < keep))
        printf("  %s run %2d (%s, %s): %s; %s; xts output %s%s\n", S->id, r, layouts[l], faults, outcome,
               line[0] ? line + 9 : "fat_check failed", (const char *[]){ "identical", "partial", "CORRUPT", "absent", "unreadable" }[xs < 0 || xs > 4 ? 4 : xs],
               silent ? "  <- silent corruption" : "");
      if (notable && S->kept < keep) {
        char kept[800];
        snprintf(kept, sizeof kept, "%s/keep/%s-run%02d-%s.img", work, S->id, r, layouts[l]);
        copy_sparse(img, kept);
        sh("cp '%s' '%s/keep/%s-run%02d.stats' 2>/dev/null; cp '%s' '%s/keep/%s-run%02d.log' 2>/dev/null", stats, work, S->id, r,
           log, work, S->id, r);
        S->kept++;
      }
    }
    if (S->patched_stack && (S->preexisting || S->silent_corrupt)) bad = 1;
  }

  /* ---- report ---- */
  char sp[700];
  snprintf(sp, sizeof sp, "%s/summary.txt", work);
  FILE *out = fopen(sp, "w");
  for (int pass = 0; pass < 2; pass++) {
    FILE *f = pass ? out : stdout;
    if (!f) continue;
    fprintf(f, "\nhost-fatreplay, profile %s, layout %s: \"1 ...\" = one fault per run at a random device call (of ~%ld; "
            "upstream libfat ~%ld), \"x%%\" = every device call of that kind faulty with that probability\n",
            xts_profile.name, layout_opt, ref_calls[0][0], ref_calls[1][0]);
    fprintf(f, "%-3s %-56s %4s %5s | %9s %5s %7s %8s %6s %5s | %8s %7s %7s %7s %6s %8s %4s %5s | %6s %6s\n", "", "scenario",
            "runs", "flts", "completed", "werr", "partial", "poisoned", "halted", "crash", "FS-error", "outside", "pre-dmg",
            "foreign", "USBblk", "dangling", "lost", "FAT2", "silent", "reread");
    for (int si = 0; si < NSCEN; si++) {
      scen_t *S = &scen[si];
      if (!S->runs) continue;
      fprintf(f, "%-3s %-56s %4d %5d | %9d %5d %7d %8d %6d %5d | %8d %7d %7d %7d %6d %8d %4d %2d/%-2d| %6d %6d\n", S->id,
              S->name, S->runs, S->faults_total, S->completed, S->io_reported, S->partial, S->poisoned,
              S->halted + S->mount_failed, S->crashed + S->hung, S->fs_errors, S->outside, S->preexisting, S->foreign_page,
              S->usb_garbage, S->dangling, S->lost, S->fat_mismatch, S->fat2_pre, S->silent_corrupt, S->read_recovered);
    }
    fprintf(f, "columns: flts = faults injected over all runs; completed = run finished (werr: of those, XTS counted write "
               "errors);\n  partial = XTS output is a prefix of the reference; poisoned = the disc guard latched the error and "
               "XTS stopped;\n  FS-error = fat_check found a structural error (stale FAT #2 not counted: column FAT2), outside = "
               "one outside /xts (FSInfo included); pre-dmg = a pre-existing entry/content/chain\n  changed or a page of another cluster was written over a pre-existing "
               "directory (foreign); USBblk = a CBW/CSW at the start\n  of a pre-existing sector; dangling = an entry points to a cluster that is free in the FAT (cross-link hazard, the\n  "
               "incident's xts entry); lost = lost clusters; FAT2 = runs with "
               "FAT #1 != FAT #2 / of those, on pre-existing files' clusters (FAT #1\n  is intact: repairable); silent = "
               "completed without any reported error but the stick or the XTS output\n  is wrong; reread = runs where the "
               "guard's second double-read repaired a bad read.\n");
    for (int si = 0; si < NSCEN; si++) {
      scen_t *S = &scen[si];
      if (!S->runs || !S->patched_stack) continue;
      fprintf(f, "verdict %s: %s\n", S->id, (S->preexisting || S->silent_corrupt) ? "UNSAFE (pre-existing damage or silent corruption)"
                                                                                  : "safe (no pre-existing damage, no silent corruption)");
    }
    for (int si = 0; si < NSCEN; si++) {
      scen_t *S = &scen[si];
      if (S->runs && S->upstream)
        fprintf(f, "verdict %s: incident class %s (%d/%d runs damaged pre-existing content, %d with a foreign page)\n", S->id,
                S->preexisting ? "REPRODUCED" : "not reproduced", S->preexisting, S->runs, S->foreign_page);
    }
  }
  if (out) fclose(out);
  printf("(images of damaged runs: %s/keep, summary: %s)\n", work, sp);
  return bad;
}
