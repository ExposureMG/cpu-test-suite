/* make host-test: runs every HTEST registered in section xts_htest (the files in host/tests). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xts/xts.h>
#include <xts/net.h>
#include "tests/htest.h"

extern const htest_t *const __start_xts_htest[] __attribute__((weak));
extern const htest_t *const __stop_xts_htest[] __attribute__((weak));
int ht_failures, ht_checks;
const char *ht_tmp = "build/host-test";

const char *ht_slurp(const char *path)
{
  static char *buf;
  free(buf);
  buf = NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t r = fread(buf, 1, (size_t)n, f);
  buf[r] = 0;
  fclose(f);
  return buf;
}

int ht_count_substr(const char *hay, const char *needle)
{
  int n = 0;
  size_t l = strlen(needle);
  for (const char *p = hay; p && (p = strstr(p, needle)); p += l) n++;
  return n;
}

const char *ht_run_file(const char *rundir, const char *file)
{
  char p[1200];
  snprintf(p, sizeof p, "%s/%s", rundir, file);
  return ht_slurp(p);
}

static int wait_pid(pid_t pid, int timeout_ms)
{
  int st = 0;
  for (int t = 0; t < timeout_ms; t += 10) {
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (r < 0) return -1;
    usleep(10000);
  }
  kill(pid, SIGTERM);                                  /* the receiver writes what it has on SIGTERM */
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int ht_recv_run(const char *name, int (*fn)(void *), void *ud, const char *env, char *rundir, size_t cap, int *recv_rc)
{
  char base[600], cmd[800], portfile[700], runp[700], logp[700];
  snprintf(base, sizeof base, "%s/%s", ht_tmp, name);
  snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", base, base);
  if (system(cmd) != 0) return -1;
  snprintf(portfile, sizeof portfile, "%s/port", base);
  snprintf(runp, sizeof runp, "%s/run", base);
  snprintf(logp, sizeof logp, "%s/recv.log", base);
  snprintf(rundir, cap, "%s", runp);
  const char *py = getenv("XTS_PYTHON"), *recv = getenv("XTS_RECV");
  if (!py) py = "python3";
  if (!recv) recv = "tools/xts_recv.py";
  fflush(NULL);
  pid_t rp = fork();
  if (rp == 0) {
    FILE *lf = freopen(logp, "w", stdout);
    (void)lf;
    dup2(fileno(stdout), 2);
    execlp(py, py, "-B", recv, "--once", "--quiet", "--port", "0", "--bind", "127.0.0.1", "--port-file", portfile,
           "--run-dir", runp, "--timeout", "3", (char *)NULL);
    _exit(127);
  }
  int port = 0;
  for (int t = 0; t < 1000 && !port; t++) {
    FILE *f = fopen(portfile, "r");
    if (f) {
      if (fscanf(f, "%d", &port) != 1) port = 0;
      fclose(f);
    }
    if (!port) usleep(10000);
  }
  if (!port) {
    printf("    receiver did not start (see %s)\n", logp);
    kill(rp, SIGKILL);
    waitpid(rp, NULL, 0);
    return -1;
  }
  pid_t cp = fork();
  if (cp == 0) {
    char host[64], e[512];
    snprintf(host, sizeof host, "127.0.0.1:%d", port);
    setenv("XTS_NET_HOST", host, 1);
    setenv("XTS_QUIET", "1", 1);
    if (env) {
      snprintf(e, sizeof e, "%s", env);
      for (char *save = NULL, *kv = strtok_r(e, ";", &save); kv; kv = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(kv, '=');
        if (eq) { *eq = 0; setenv(kv, eq + 1, 1); }
      }
    }
    char *argv[] = { "x", "--quiet", NULL };
    xts_plat_init(2, argv);
    if (xts_net_init() < 0) _exit(126);
    int rc = fn(ud);
    fflush(NULL);
    _exit(rc & 0xFF);
  }
  int cst = 0;
  waitpid(cp, &cst, 0);
  int rrc = wait_pid(rp, 30000);
  if (recv_rc) *recv_rc = rrc;
  return WIFEXITED(cst) ? WEXITSTATUS(cst) : -1;
}

int main(int argc, char **argv)
{
  const char *only = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--tmp") && i + 1 < argc) ht_tmp = argv[++i];
    else only = argv[i];
  }
  setenv("XTS_QUIET", "1", 1);
  mkdir(ht_tmp, 0777);
  int ntests = 0, nfail = 0;
  for (const htest_t *const *p = __start_xts_htest; p && p < __stop_xts_htest; p++) {
    if (only && strcmp(only, (*p)->name)) continue;
    int before = ht_failures, checks = ht_checks;
    (*p)->fn();
    ntests++;
    int failed = ht_failures != before;
    nfail += failed;
    printf("  %-28s %s (%d checks)\n", (*p)->name, failed ? "FAIL" : "ok", ht_checks - checks);
  }
  printf("host-test: %d tests, %d checks, %d failed checks -> %s\n", ntests, ht_checks, ht_failures,
         nfail ? "FAIL" : "PASS");
  return nfail ? 1 : 0;
}
