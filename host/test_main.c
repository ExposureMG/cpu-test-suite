/* make host-test: runs every HTEST registered in section xts_htest (the files in host/tests). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <xts/xts.h>
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
